#include <errno.h>
#include <dirent.h>
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

enum color_index {
    COLOR_TEXT,
    COLOR_BATTERY,
    COLOR_DATE,
    COLOR_SECONDS,
    COLOR_WORKSPACE,
    COLOR_LIGHT_TEXT,
    COLOR_LIGHT_BATTERY,
    COLOR_LIGHT_DATE,
    COLOR_LIGHT_SECONDS,
    COLOR_LIGHT_WORKSPACE,
    COLOR_DARK_TEXT,
    COLOR_DARK_BATTERY,
    COLOR_DARK_DATE,
    COLOR_DARK_SECONDS,
    COLOR_DARK_WORKSPACE,
    COLOR_COUNT,
};

static XRenderColor light_equivalent(XRenderColor color) {
    const unsigned int lift = 0x4000;

    color.red = color.red > 0xffff - lift ? 0xffff : color.red + lift;
    color.green = color.green > 0xffff - lift ? 0xffff : color.green + lift;
    color.blue = color.blue > 0xffff - lift ? 0xffff : color.blue + lift;
    return color;
}

static XRenderColor dark_equivalent(XRenderColor color, enum color_index base) {
    unsigned short level;

    switch (base) {
    case COLOR_TEXT:
        level = 0x0000;
        break;
    case COLOR_BATTERY:
        level = 0x2020;
        break;
    case COLOR_DATE:
        level = 0x5050;
        break;
    case COLOR_SECONDS:
        level = 0x7070;
        break;
    case COLOR_WORKSPACE:
        level = 0x9090;
        break;
    default:
        level = color.red / 2;
        break;
    }
    color.red = color.green = color.blue = level;
    return color;
}

static int allocate_color(Display *display, Visual *visual, Colormap colormap,
                          XRenderColor color, XftColor *result) {
    return XftColorAllocValue(display, visual, colormap, &color, result) ? 0 : -1;
}

static int read_power_supply_value(const char *battery, const char *name, long long *value) {
    char path[256];
    FILE *file;

    if (snprintf(path, sizeof(path), "/sys/class/power_supply/%s/%s", battery, name)
        >= (int)sizeof(path))
        return -1;
    file = fopen(path, "r");
    if (file == NULL || fscanf(file, "%lld", value) != 1) {
        if (file != NULL)
            fclose(file);
        return -1;
    }
    fclose(file);
    return 0;
}

static int battery_remaining_time(char *battery_text, size_t size) {
    DIR *directory;
    struct dirent *entry;
    char type[32];
    char status[32];
    long long remaining;
    long long rate;

    battery_text[0] = '\0';
    directory = opendir("/sys/class/power_supply");
    if (directory == NULL)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        char path[256];
        FILE *file;
        long long seconds;
        long long hours;
        long long minutes;

        if (entry->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name)
            >= (int)sizeof(path))
            continue;
        file = fopen(path, "r");
        if (file == NULL || fgets(type, sizeof(type), file) == NULL) {
            if (file != NULL)
                fclose(file);
            continue;
        }
        fclose(file);
        if (strcmp(type, "Battery\n") != 0)
            continue;
        if (snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", entry->d_name)
            >= (int)sizeof(path))
            continue;
        file = fopen(path, "r");
        if (file == NULL || fgets(status, sizeof(status), file) == NULL) {
            if (file != NULL)
                fclose(file);
            continue;
        }
        fclose(file);
        if (strcmp(status, "Discharging\n") != 0)
            continue;
        if (read_power_supply_value(entry->d_name, "energy_now", &remaining) != 0
            || read_power_supply_value(entry->d_name, "power_now", &rate) != 0) {
            if (read_power_supply_value(entry->d_name, "charge_now", &remaining) != 0
                || read_power_supply_value(entry->d_name, "current_now", &rate) != 0)
                continue;
        }
        if (remaining < 0 || rate <= 0)
            continue;
        seconds = (remaining * 3600) / rate;
        if (seconds >= 60 * 60)
            continue;
        hours = seconds / 3600;
        minutes = (seconds % 3600) / 60;
        if (snprintf(battery_text, size, "%02lld%02lld", hours, minutes) >= (int)size) {
            closedir(directory);
            return -1;
        }
        closedir(directory);
        return 0;
    }
    closedir(directory);
    return -1;
}

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

static unsigned long pixel_component(unsigned long pixel, unsigned long mask) {
    unsigned long maximum;
    unsigned int shift = 0;

    if (mask == 0)
        return 0;
    while ((mask & 1) == 0) {
        mask >>= 1;
        shift++;
    }
    maximum = mask;
    return (((pixel >> shift) & maximum) * 0xffff) / maximum;
}

static int root_background_is_light(Display *display, Window root, Visual *visual, int x,
                                    int y, int width, int height) {
    unsigned long luminance = 0;
    unsigned long samples = 0;
    XImage *image;

    if (width <= 0 || height <= 0)
        return 0;
    image = XGetImage(display, root, x, y, (unsigned int)width, (unsigned int)height,
                      AllPlanes, ZPixmap);
    if (image == NULL)
        return 0;
    for (int row = 0; row < height; row += 4) {
        for (int column = 0; column < width; column += 4) {
            unsigned long pixel = XGetPixel(image, column, row);
            unsigned long red = pixel_component(pixel, visual->red_mask);
            unsigned long green = pixel_component(pixel, visual->green_mask);
            unsigned long blue = pixel_component(pixel, visual->blue_mask);

            luminance += (red * 2126 + green * 7152 + blue * 722) / 10000;
            samples++;
        }
    }
    XDestroyImage(image);
    return samples != 0 && luminance / samples >= 0x8000;
}

static XftColor *span_color(Display *display, Window root, Visual *root_visual,
                            XftColor *colors, enum color_index base, int x, int y,
                            int width, int height) {
    if (!CCLOCK_DYNAMIC_COLOURS)
        return &colors[base];
    if (root_background_is_light(display, root, root_visual, x, y, width, height))
        return &colors[base + COLOR_DARK_TEXT - COLOR_TEXT];
    return &colors[base + COLOR_LIGHT_TEXT - COLOR_TEXT];
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

static int draw_span(Display *display, Window root, Visual *root_visual, XftDraw *draw,
                     XftFont *font, XftColor *colors, enum color_index base,
                     const char *text, int root_x, int root_y, int x, int y,
                     int window_height) {
    int width = 0;
    int height = 0;

    if (text[0] == '\0')
        return x;
    measure_text(display, font, text, &width, &height);
    return draw_text(display, draw, font,
                     span_color(display, root, root_visual, colors, base, root_x + x,
                                root_y, width, window_height),
                     text, x, y);
}

static void draw_clock(Display *display, Window window, XftDraw *draw, XftFont *font,
                       Window root, Visual *root_visual, XftColor *colors,
                       const char *workspace, const char *battery, const char *date,
                       const char *time_text, const char *seconds, const char *text,
                       int window_width, int window_height) {
    int text_width = 0;
    int text_height = 0;
    measure_text(display, font, text, &text_width, &text_height);

    int baseline = CCLOCK_PADDING_Y + font->ascent;
    int x = window_width - text_width - CCLOCK_PADDING_X;
    int y = baseline;
    int root_x = DisplayWidth(display, DefaultScreen(display)) - window_width
        - CCLOCK_MARGIN_RIGHT;
    int root_y = DisplayHeight(display, DefaultScreen(display)) - window_height
        - CCLOCK_MARGIN_BOTTOM;

    XClearWindow(display, window);
    x = draw_span(display, root, root_visual, draw, font, colors, COLOR_WORKSPACE,
                  workspace, root_x, root_y, x, y, window_height);
    if (workspace[0] != '\0')
        x = draw_span(display, root, root_visual, draw, font, colors, COLOR_TEXT,
                      CCLOCK_WORKSPACE_SEPARATOR, root_x, root_y, x, y, window_height);
    x = draw_span(display, root, root_visual, draw, font, colors, COLOR_BATTERY, battery,
                  root_x, root_y, x, y, window_height);
    x = draw_span(display, root, root_visual, draw, font, colors, COLOR_DATE, date,
                  root_x, root_y, x, y, window_height);
    x = draw_span(display, root, root_visual, draw, font, colors, COLOR_TEXT, time_text,
                  root_x, root_y, x, y, window_height);
    draw_span(display, root, root_visual, draw, font, colors, COLOR_SECONDS, seconds,
              root_x, root_y, x, y, window_height);
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

    XRenderColor configured_colors[COLOR_WORKSPACE + 1] = {
        [COLOR_TEXT] = { CCLOCK_TEXT_RED, CCLOCK_TEXT_GREEN, CCLOCK_TEXT_BLUE,
                         CCLOCK_TEXT_ALPHA },
        [COLOR_BATTERY] = { CCLOCK_BATTERY_RED, CCLOCK_BATTERY_GREEN,
                            CCLOCK_BATTERY_BLUE, CCLOCK_BATTERY_ALPHA },
        [COLOR_DATE] = { CCLOCK_DATE_RED, CCLOCK_DATE_GREEN, CCLOCK_DATE_BLUE,
                         CCLOCK_DATE_ALPHA },
        [COLOR_SECONDS] = { CCLOCK_SECONDS_RED, CCLOCK_SECONDS_GREEN,
                            CCLOCK_SECONDS_BLUE, CCLOCK_SECONDS_ALPHA },
        [COLOR_WORKSPACE] = { CCLOCK_WORKSPACE_RED, CCLOCK_WORKSPACE_GREEN,
                              CCLOCK_WORKSPACE_BLUE, CCLOCK_WORKSPACE_ALPHA },
    };
    XRenderColor color_values[COLOR_COUNT];
    XftColor colors[COLOR_COUNT];
    int allocated_colors = 0;

    for (int i = COLOR_TEXT; i <= COLOR_WORKSPACE; ++i) {
        color_values[i] = configured_colors[i];
        color_values[COLOR_LIGHT_TEXT + i] = light_equivalent(configured_colors[i]);
        color_values[COLOR_DARK_TEXT + i] = dark_equivalent(configured_colors[i], i);
    }
    for (; allocated_colors < COLOR_COUNT; ++allocated_colors) {
        if (allocate_color(display, visual, colormap, color_values[allocated_colors],
                           &colors[allocated_colors]) == 0)
            continue;
        fputs("failed to allocate clock color\n", stderr);
        while (allocated_colors > 0) {
            allocated_colors--;
            XftColorFree(display, visual, colormap, &colors[allocated_colors]);
        }
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
    char previous_battery[CCLOCK_BATTERY_TEXT_BUFFER_SIZE];
    char previous_date[CCLOCK_DATE_TEXT_BUFFER_SIZE];
    char previous_time[CCLOCK_TEXT_BUFFER_SIZE];
    char previous_seconds[CCLOCK_SECONDS_TEXT_BUFFER_SIZE];
    previous_workspace[0] = previous_battery[0] = previous_date[0] = previous_time[0]
        = previous_seconds[0] = '\0';
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
                    draw_clock(display, window, draw, font, RootWindow(display, screen),
                               DefaultVisual(display, screen), colors, previous_workspace,
                               previous_battery, previous_date, previous_time,
                               previous_seconds, previous_text, width, height);
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
        char battery[CCLOCK_BATTERY_TEXT_BUFFER_SIZE];
        battery_remaining_time(battery, sizeof(battery));
        char text[CCLOCK_RENDER_TEXT_BUFFER_SIZE];
        int written = workspace[0]
                          ? snprintf(text, sizeof(text), "%s%s%s%s%s%s", workspace,
                                     CCLOCK_WORKSPACE_SEPARATOR, battery, date, time_text,
                                     seconds)
                          : snprintf(text, sizeof(text), "%s%s%s%s", battery, date,
                                     time_text, seconds);
        if (written < 0 || (size_t)written >= sizeof(text)) {
            fputs("battery, workspace, and clock text exceed CCLOCK_RENDER_TEXT_BUFFER_SIZE\n",
                  stderr);
            break;
        }

        if (visible && (CCLOCK_DYNAMIC_COLOURS || needs_redraw
                        || strcmp(text, previous_text) != 0)) {
            resize_for_text(display, screen, window, font, text, &width, &height);
            draw_clock(display, window, draw, font, RootWindow(display, screen),
                       DefaultVisual(display, screen), colors, workspace, battery, date,
                       time_text, seconds, text, width, height);
            needs_redraw = false;
        }
        snprintf(previous_text, sizeof(previous_text), "%s", text);
        snprintf(previous_workspace, sizeof(previous_workspace), "%s", workspace);
        snprintf(previous_battery, sizeof(previous_battery), "%s", battery);
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

    while (allocated_colors > 0) {
        allocated_colors--;
        XftColorFree(display, visual, colormap, &colors[allocated_colors]);
    }
    XftFontClose(display, font);
    XftDrawDestroy(draw);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
    close(control_fd);
    unlink(control_path);
    return 0;
}
