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

int main(void) {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

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

    int width = 180;
    int height = 42;
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    int margin_x = 16;
    int margin_y = 10;

    XSetWindowAttributes attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.colormap = colormap;
    attributes.background_pixel = 0x00000000;
    attributes.border_pixel = 0;
    attributes.override_redirect = True;

    Window window = XCreateWindow(
        display, RootWindow(display, screen), screen_width - width - margin_x,
        screen_height - height - margin_y, (unsigned int)width, (unsigned int)height, 0,
        depth, InputOutput, visual,
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

    XMapRaised(display, window);

    XftDraw *draw = XftDrawCreate(display, window, visual, colormap);
    if (draw == NULL) {
        fputs("failed to create Xft draw surface\n", stderr);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        return 1;
    }

    XftFont *font = XftFontOpenName(display, screen, "monospace-24");
    if (font == NULL) {
        fputs("failed to load font monospace-24\n", stderr);
        XftDrawDestroy(draw);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XCloseDisplay(display);
        return 1;
    }

    XRenderColor text_color = {
        .red = 0xffff,
        .green = 0xa800,
        .blue = 0x0000,
        .alpha = 0xd000,
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

    char previous_text[16] = "";

    while (keep_running) {
        time_t now = time(NULL);
        struct tm local_time;
        localtime_r(&now, &local_time);

        char text[16];
        strftime(text, sizeof(text), "%H:%M:%S", &local_time);

        if (strcmp(text, previous_text) != 0) {
            XClearWindow(display, window);

            XGlyphInfo extents;
            XftTextExtentsUtf8(display, font, (const FcChar8 *)text,
                               (int)strlen(text), &extents);
            int x = width - extents.xOff - 8;
            int y = font->ascent + 4;

            XftDrawStringUtf8(draw, &color, font, x, y, (const FcChar8 *)text,
                              (int)strlen(text));
            XFlush(display);
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
