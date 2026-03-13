#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include "config.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signum) {
    (void)signum;
    keep_running = 0;
}

static int sleep_until_next_second(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        perror("clock_gettime");
        return -1;
    }

    struct timespec target = {
        .tv_sec = now.tv_sec + 1,
        .tv_nsec = 0,
    };

    while (keep_running) {
        int rc = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &target, NULL);
        if (rc == 0) {
            return 0;
        }
        if (rc != EINTR) {
            fprintf(stderr, "clock_nanosleep failed: %d\n", rc);
            return -1;
        }
    }

    return 0;
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

static void draw_clock(Display *display, Window window, XftDraw *draw, XftFont *font,
                       XftColor *color, const char *text, int window_width,
                       int window_height) {
    int text_width = 0;
    int text_height = 0;
    measure_text(display, font, text, &text_width, &text_height);

    int baseline = CCLOCK_PADDING_Y + font->ascent;
    int x = window_width - text_width - CCLOCK_PADDING_X;
    int y = baseline;

    XClearWindow(display, window);
    XftDrawStringUtf8(draw, color, font, x, y, (const FcChar8 *)text, (int)strlen(text));
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
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (validate_config() != 0) {
        return 1;
    }

    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fputs("failed to open X display\n", stderr);
        return 1;
    }

    int screen = DefaultScreen(display);
    int depth = 0;
    Visual *visual = find_argb_visual(display, screen, &depth);
    if (visual == NULL) {
        fputs("failed to find 32-bit ARGB visual\n", stderr);
        XCloseDisplay(display);
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

    XftDraw *draw = XftDrawCreate(display, window, visual, colormap);
    if (draw == NULL) {
        fputs("failed to create Xft draw surface\n", stderr);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        return 1;
    }

    XftFont *font = XftFontOpenName(display, screen, CCLOCK_FONT_NAME);
    if (font == NULL) {
        fprintf(stderr, "failed to load font %s\n", CCLOCK_FONT_NAME);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        return 1;
    }

    int sample_width = 0;
    int sample_height = 0;
    measure_text(display_for_metrics, font, CCLOCK_LAYOUT_TEXT, &sample_width,
                 &sample_height);
    width = sample_width + (CCLOCK_PADDING_X * 2);
    height = sample_height + (CCLOCK_PADDING_Y * 2);
    move_to_bottom_right(display, screen, window, width, height);

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
        return 1;
    }

    XMapRaised(display, window);

    char previous_text[CCLOCK_TEXT_BUFFER_SIZE];
    previous_text[0] = '\0';

    while (keep_running) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ConfigureNotify) {
                move_to_bottom_right(display, screen, window, width, height);
            }
            if (event.type == Expose) {
                if (previous_text[0] != '\0') {
                    draw_clock(display, window, draw, font, &color, previous_text, width,
                               height);
                }
            }
        }

        time_t now = time(NULL);
        struct tm local_time;
        if (localtime_r(&now, &local_time) == NULL) {
            perror("localtime_r");
            break;
        }

        char text[CCLOCK_TEXT_BUFFER_SIZE];
        if (strftime(text, sizeof(text), CCLOCK_TIME_FORMAT, &local_time) == 0) {
            fputs("strftime failed\n", stderr);
            break;
        }

        if (strcmp(text, previous_text) != 0) {
            draw_clock(display, window, draw, font, &color, text, width, height);
            memcpy(previous_text, text, sizeof(text));
        }

        if (sleep_until_next_second() != 0) {
            break;
        }
    }

    XftColorFree(display, visual, colormap, &color);
    XftFontClose(display, font);
    XftDrawDestroy(draw);
    XDestroyWindow(display, window);
    XFreeColormap(display, colormap);
    XCloseDisplay(display);
    return 0;
}
