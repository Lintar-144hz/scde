/*
 * Minimal X client used by the SCDE test suite.
 * usage: testclient [-g WIDTHxHEIGHT+X+Y] [-n NAME]
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    Display *dpy;
    Window win;
    XEvent ev;
    int screen, w = 320, h = 220, x = 60, y = 60;
    const char *name = "scde-test-client";
    int i;
    Atom wm_delete;
    XSizeHints hints;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-g") == 0) {
            sscanf(argv[i + 1], "%dx%d+%d+%d", &w, &h, &x, &y);
            i++;
        } else if (strcmp(argv[i], "-n") == 0) {
            name = argv[i + 1];
            i++;
        }
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "testclient: no display\n");
        return 1;
    }
    screen = DefaultScreen(dpy);

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), x, y,
                              (unsigned)w, (unsigned)h, 1,
                              BlackPixel(dpy, screen),
                              WhitePixel(dpy, screen));
    XStoreName(dpy, win, name);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);

    hints.flags = PPosition | PSize;
    hints.x = x;
    hints.y = y;
    hints.width = w;
    hints.height = h;
    XSetWMNormalHints(dpy, win, &hints);

    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XMapWindow(dpy, win);
    XFlush(dpy);

    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            XSetForeground(dpy, DefaultGC(dpy, screen), WhitePixel(dpy, screen));
            XFillRectangle(dpy, win, DefaultGC(dpy, screen), 0, 0,
                           (unsigned)w, (unsigned)h);
        } else if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == wm_delete)
                break;
        } else if (ev.type == DestroyNotify) {
            break;
        }
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
