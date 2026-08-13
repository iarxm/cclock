#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include "config.h"

static volatile sig_atomic_t keep_running = 1;
static const time_t STACK_REFRESH_INTERVAL_SECONDS = 5 * 60;

static void handle_signal(int signum) {
    (void)signum;
    keep_running = 0;
}

static int socket_path(char *path, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int written;

    if (runtime && runtime[0])
        written = snprintf(path, size, "%s/%s", runtime, CCLOCK_SOCKET_NAME);
    else
        written = snprintf(path, size, "/tmp/cclock-%ld.sock", (long)getuid());
    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int create_control_socket(const char *path) {
    struct sockaddr_un address;
    struct stat st;
    int fd;

    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
            fprintf(stderr, "refusing to replace %s\n", path);
            return -1;
        }
        if (unlink(path) < 0) {
            perror("unlink control socket");
            return -1;
        }
    } else if (errno != ENOENT) {
        perror("lstat control socket");
        return -1;
    }
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 4) < 0) {
        perror("bind control socket");
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static void handle_control(int control_fd, Display *display, Window window, int *visible,
                           bool *needs_redraw) {
    char command[32];
    bool state_changed = false;
    int client_fd;
    ssize_t n;

    if ((client_fd = accept(control_fd, NULL, NULL)) < 0)
        return;
    n = read(client_fd, command, sizeof(command) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    command[n] = '\0';
    if (!strcmp(command, "toggle\n")) {
        *visible = !*visible;
        state_changed = true;
    } else if (!strcmp(command, "show\n")) {
        *visible = 1;
        state_changed = true;
    } else if (!strcmp(command, "hide\n")) {
        *visible = 0;
        state_changed = true;
    } else if (strcmp(command, "status\n")) {
        dprintf(client_fd, "error\n");
        close(client_fd);
        return;
    }
    if (state_changed && *visible) {
        XMapRaised(display, window);
        *needs_redraw = true;
    } else if (state_changed) {
        XUnmapWindow(display, window);
    }
    dprintf(client_fd, "%s\n", *visible ? "shown" : "hidden");
    XFlush(display);
    close(client_fd);
}

static void update_workspace(Display *display, Window root, Atom workspace_atom,
                             char *workspace, size_t size) {
    unsigned char *value = NULL;
    unsigned long bytes_after;
    unsigned long items;
    Atom actual_type;
    int actual_format;

    if (XGetWindowProperty(display, root, workspace_atom, 0L, (long)size - 1, False,
                           AnyPropertyType, &actual_type, &actual_format, &items,
                           &bytes_after, &value) != Success || actual_format != 8
        || value == NULL) {
        workspace[0] = '\0';
    } else {
        size_t length = items < size - 1 ? items : size - 1;
        memcpy(workspace, value, length);
        workspace[length] = '\0';
    }
    if (value != NULL)
        XFree(value);
    (void)actual_type;
    (void)bytes_after;
}

static Visual *find_argb_visual(Display *display, int screen, int *depth) {
    XVisualInfo template = {
        .screen = screen,
        .depth = 32,
        .class = TrueColor,
    };
    int count = 0;
    XVisualInfo *infos =
        XGetVisualInfo(display, VisualScreenMask | VisualDepthMask | VisualClassMask,
                       &template, &count);
    if (infos == NULL) {
        return NULL;
    }

    Visual *visual = NULL;
    for (int i = 0; i < count; ++i) {
        XRenderPictFormat *format =
            XRenderFindVisualFormat(display, infos[i].visual);
        if (format != NULL && format->type == PictTypeDirect && format->direct.alphaMask) {
            visual = infos[i].visual;
            *depth = infos[i].depth;
            break;
        }
    }

    XFree(infos);
    return visual;
}

static void set_window_type(Display *display, Window window, Atom property, const char *type) {
    Atom value = XInternAtom(display, type, False);
    XChangeProperty(display, window, property, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&value, 1);
}

static void measure_text(Display *display, XftFont *font, const char *text,
                         int *text_width, int *text_height) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(display, font, (const FcChar8 *)text, (int)strlen(text), &extents);
    *text_width = extents.xOff;
    *text_height = font->ascent + font->descent;
}

static void move_to_bottom_right(Display *display, int screen, Window window,
                                 int window_width, int window_height) {
    int x = DisplayWidth(display, screen) - window_width - CCLOCK_MARGIN_RIGHT;
    int y = DisplayHeight(display, screen) - window_height - CCLOCK_MARGIN_BOTTOM;
    XMoveResizeWindow(display, window, x, y, (unsigned int)window_width,
                      (unsigned int)window_height);
}

static void resize_for_text(Display *display, int screen, Window window, XftFont *font,
                            const char *text, int *window_width, int *window_height) {
    int text_width = 0;
    int text_height = 0;

    measure_text(display, font, text, &text_width, &text_height);
    *window_width = text_width + (CCLOCK_PADDING_X * 2);
    *window_height = text_height + (CCLOCK_PADDING_Y * 2);
    move_to_bottom_right(display, screen, window, *window_width, *window_height);
}

static int draw_text(Display *display, XftDraw *draw, XftFont *font, XftColor *color,
                     const char *text, int x, int y) {
    int text_width = 0;
    int text_height = 0;

    if (text[0] == '\0')
        return x;
    measure_text(display, font, text, &text_width, &text_height);
    XftDrawStringUtf8(draw, color, font, x, y, (const FcChar8 *)text, (int)strlen(text));
    (void)display;
    (void)text_height;
    return x + text_width;
}

static void draw_clock(Display *display, Window window, XftDraw *draw, XftFont *font,
                       XftColor *color, XftColor *date_color, XftColor *seconds_color,
                       XftColor *workspace_color, const char *workspace, const char *date,
                       const char *time_text, const char *seconds, const char *text,
                       int window_width, int window_height) {
    int text_width = 0;
    int text_height = 0;
    measure_text(display, font, text, &text_width, &text_height);

    int baseline = CCLOCK_PADDING_Y + font->ascent;
    int x = window_width - text_width - CCLOCK_PADDING_X;
    int y = baseline;

    XClearWindow(display, window);
    x = draw_text(display, draw, font, workspace_color, workspace, x, y);
    if (workspace[0] != '\0')
        x = draw_text(display, draw, font, color, CCLOCK_WORKSPACE_SEPARATOR, x, y);
    x = draw_text(display, draw, font, date_color, date, x, y);
    x = draw_text(display, draw, font, color, time_text, x, y);
    draw_text(display, draw, font, seconds_color, seconds, x, y);
    XFlush(display);
    (void)window_height;
}

static int validate_config(void) {
    if (strlen(CCLOCK_LAYOUT_TEXT) + 1 > CCLOCK_TEXT_BUFFER_SIZE) {
        fprintf(stderr,
                "configured layout text exceeds CCLOCK_TEXT_BUFFER_SIZE (%d)\n",
                CCLOCK_TEXT_BUFFER_SIZE);
        return -1;
    }

    return 0;
}

int main(void) {
    char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (validate_config() != 0) {
        return 1;
    }
    if (socket_path(control_path, sizeof(control_path)) < 0) {
        fputs("cclock socket path is too long\n", stderr);
        return 1;
    }
    umask(0077);
    int control_fd = create_control_socket(control_path);
    if (control_fd < 0)
        return 1;

    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fputs("failed to open X display\n", stderr);
        close(control_fd);
        unlink(control_path);
        return 1;
    }

    int screen = DefaultScreen(display);
    int depth = 0;
    Visual *visual = find_argb_visual(display, screen, &depth);
    if (visual == NULL) {
        fputs("failed to find 32-bit ARGB visual\n", stderr);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }

    Colormap colormap =
        XCreateColormap(display, RootWindow(display, screen), visual, AllocNone);

    Display *display_for_metrics = display;

    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap = colormap;
    attributes.background_pixel = 0x00000000;
    attributes.border_pixel = 0;
    attributes.override_redirect = True;

    int width = 1;
    int height = 1;
    Window window = XCreateWindow(
        display, RootWindow(display, screen), 0, 0, (unsigned int)width,
        (unsigned int)height, 0, depth, InputOutput, visual,
        CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect, &attributes);

    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom states[2] = {
        XInternAtom(display, "_NET_WM_STATE_ABOVE", False),
        XInternAtom(display, "_NET_WM_STATE_STICKY", False),
    };
    XChangeProperty(display, window, net_wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 2);

    set_window_type(display, window, XInternAtom(display, "_NET_WM_WINDOW_TYPE", False),
                    "_NET_WM_WINDOW_TYPE_DOCK");

    XRectangle empty_rect = {0, 0, 0, 0};
    XserverRegion region = XFixesCreateRegion(display, &empty_rect, 0);
    XFixesSetWindowShapeRegion(display, window, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(display, region);

    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    Atom workspace_atom = XInternAtom(display, "_CCLOCK_WORKSPACE", False);
    XSelectInput(display, RootWindow(display, screen), PropertyChangeMask);

    XftDraw *draw = XftDrawCreate(display, window, visual, colormap);
    if (draw == NULL) {
        fputs("failed to create Xft draw surface\n", stderr);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }

    XftFont *font = XftFontOpenName(display, screen, CCLOCK_FONT_NAME);
    if (font == NULL) {
        fprintf(stderr, "failed to load font %s\n", CCLOCK_FONT_NAME);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }

    resize_for_text(display_for_metrics, screen, window, font, CCLOCK_LAYOUT_TEXT, &width,
                    &height);

    XRenderColor text_color = {
        .red = CCLOCK_TEXT_RED,
        .green = CCLOCK_TEXT_GREEN,
        .blue = CCLOCK_TEXT_BLUE,
        .alpha = CCLOCK_TEXT_ALPHA,
    };
    XftColor color;
    if (!XftColorAllocValue(display, visual, colormap, &text_color, &color)) {
        fputs("failed to allocate text color\n", stderr);
        XftFontClose(display, font);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }
    XRenderColor date_text_color = {
        .red = CCLOCK_DATE_RED,
        .green = CCLOCK_DATE_GREEN,
        .blue = CCLOCK_DATE_BLUE,
        .alpha = CCLOCK_DATE_ALPHA,
    };
    XftColor date_color;
    if (!XftColorAllocValue(display, visual, colormap, &date_text_color, &date_color)) {
        fputs("failed to allocate date text color\n", stderr);
        XftColorFree(display, visual, colormap, &color);
        XftFontClose(display, font);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }
    XRenderColor seconds_text_color = {
        .red = CCLOCK_SECONDS_RED,
        .green = CCLOCK_SECONDS_GREEN,
        .blue = CCLOCK_SECONDS_BLUE,
        .alpha = CCLOCK_SECONDS_ALPHA,
    };
    XftColor seconds_color;
    if (!XftColorAllocValue(display, visual, colormap, &seconds_text_color, &seconds_color)) {
        fputs("failed to allocate seconds text color\n", stderr);
        XftColorFree(display, visual, colormap, &date_color);
        XftColorFree(display, visual, colormap, &color);
        XftFontClose(display, font);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }
    XRenderColor workspace_text_color = {
        .red = CCLOCK_WORKSPACE_RED,
        .green = CCLOCK_WORKSPACE_GREEN,
        .blue = CCLOCK_WORKSPACE_BLUE,
        .alpha = CCLOCK_WORKSPACE_ALPHA,
    };
    XftColor workspace_color;
    if (!XftColorAllocValue(display, visual, colormap, &workspace_text_color,
                            &workspace_color)) {
        fputs("failed to allocate workspace text color\n", stderr);
        XftColorFree(display, visual, colormap, &seconds_color);
        XftColorFree(display, visual, colormap, &date_color);
        XftColorFree(display, visual, colormap, &color);
        XftFontClose(display, font);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        close(control_fd);
        unlink(control_path);
        return 1;
    }

    XMapRaised(display, window);

    char previous_text[CCLOCK_RENDER_TEXT_BUFFER_SIZE];
    previous_text[0] = '\0';
    char previous_workspace[CCLOCK_WORKSPACE_TEXT_BUFFER_SIZE];
    char previous_date[CCLOCK_DATE_TEXT_BUFFER_SIZE];
    char previous_time[CCLOCK_TEXT_BUFFER_SIZE];
    char previous_seconds[CCLOCK_SECONDS_TEXT_BUFFER_SIZE];
    previous_workspace[0] = previous_date[0] = previous_time[0] = previous_seconds[0] = '\0';
    char workspace[CCLOCK_WORKSPACE_TEXT_BUFFER_SIZE];
    update_workspace(display, RootWindow(display, screen), workspace_atom, workspace,
                     sizeof(workspace));
    time_t next_stack_refresh = time(NULL) + STACK_REFRESH_INTERVAL_SECONDS;
    bool needs_redraw = false;
    int visible = 1;

    while (keep_running) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ConfigureNotify) {
                move_to_bottom_right(display, screen, window, width, height);
            }
            if (event.type == Expose) {
                if (previous_text[0] != '\0') {
                    draw_clock(display, window, draw, font, &color, &date_color,
                               &seconds_color, &workspace_color, previous_workspace,
                               previous_date, previous_time, previous_seconds, previous_text,
                               width, height);
                }
            }
            if (event.type == PropertyNotify && event.xproperty.window == RootWindow(display, screen)
                && event.xproperty.atom == workspace_atom) {
                update_workspace(display, RootWindow(display, screen), workspace_atom,
                                 workspace, sizeof(workspace));
                needs_redraw = true;
            }
        }

        time_t now = time(NULL);
        struct tm local_time;
        if (localtime_r(&now, &local_time) == NULL) {
            perror("localtime_r");
            break;
        }

        char date[CCLOCK_DATE_TEXT_BUFFER_SIZE];
        if (strftime(date, sizeof(date), CCLOCK_DATE_FORMAT, &local_time) == 0) {
            fputs("strftime failed\n", stderr);
            break;
        }
        char time_text[CCLOCK_TEXT_BUFFER_SIZE];
        if (strftime(time_text, sizeof(time_text), CCLOCK_TIME_FORMAT, &local_time) == 0) {
            fputs("strftime failed\n", stderr);
            break;
        }
        char seconds[CCLOCK_SECONDS_TEXT_BUFFER_SIZE];
        if (strftime(seconds, sizeof(seconds), CCLOCK_SECONDS_FORMAT, &local_time) == 0) {
            fputs("strftime failed\n", stderr);
            break;
        }
        char text[CCLOCK_RENDER_TEXT_BUFFER_SIZE];
        int written = workspace[0]
                          ? snprintf(text, sizeof(text), "%s%s%s%s%s", workspace,
                                     CCLOCK_WORKSPACE_SEPARATOR, date, time_text, seconds)
                          : snprintf(text, sizeof(text), "%s%s%s", date, time_text, seconds);
        if (written < 0 || (size_t)written >= sizeof(text)) {
            fputs("workspace and clock text exceed CCLOCK_RENDER_TEXT_BUFFER_SIZE\n", stderr);
            break;
        }

        if (visible && (needs_redraw || strcmp(text, previous_text) != 0)) {
            resize_for_text(display, screen, window, font, text, &width, &height);
            draw_clock(display, window, draw, font, &color, &date_color, &seconds_color,
                       &workspace_color, workspace, date, time_text, seconds, text, width,
                       height);
            needs_redraw = false;
        }
        snprintf(previous_text, sizeof(previous_text), "%s", text);
        snprintf(previous_workspace, sizeof(previous_workspace), "%s", workspace);
        snprintf(previous_date, sizeof(previous_date), "%s", date);
        snprintf(previous_time, sizeof(previous_time), "%s", time_text);
        snprintf(previous_seconds, sizeof(previous_seconds), "%s", seconds);

        if (now >= next_stack_refresh) {
            XRaiseWindow(display, window);
            XFlush(display);
            next_stack_refresh = now + STACK_REFRESH_INTERVAL_SECONDS;
        }

        struct timespec clock_now;
        if (clock_gettime(CLOCK_REALTIME, &clock_now) != 0) {
            perror("clock_gettime");
            break;
        }
        int timeout = 1000 - (int)(clock_now.tv_nsec / 1000000);
        struct pollfd fds[] = {
            { .fd = XConnectionNumber(display), .events = POLLIN, .revents = 0 },
            { .fd = control_fd, .events = POLLIN, .revents = 0 },
        };
        int poll_result = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout);
        if (poll_result < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        if (poll_result > 0 && fds[1].revents & POLLIN)
            handle_control(control_fd, display, window, &visible, &needs_redraw);
    }

    XftColorFree(display, visual, colormap, &color);
    XftColorFree(display, visual, colormap, &date_color);
    XftColorFree(display, visual, colormap, &seconds_color);
    XftColorFree(display, visual, colormap, &workspace_color);
    XftFontClose(display, font);
    XftDrawDestroy(draw);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
    close(control_fd);
    unlink(control_path);
    return 0;
}
