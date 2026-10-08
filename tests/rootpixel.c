/*
 * Prints a root window pixel - used to verify the desktop background colour
 * set by SCDE and the desktop shortcuts drawn on the root window.
 * usage: rootpixel [x y]      (defaults to 0,0)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    Display *dpy;
    Window root;
    XImage *img;
    unsigned long pixel;
    int x = 0, y = 0;

    if (argc >= 3) {
        x = atoi(argv[1]);
        y = atoi(argv[2]);
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "rootpixel: cannot open display\n");
        return 2;
    }
    root = RootWindow(dpy, DefaultScreen(dpy));
    img = XGetImage(dpy, root, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "rootpixel: cannot read root window\n");
        XCloseDisplay(dpy);
        return 3;
    }
    pixel = XGetPixel(img, 0, 0);
    XDestroyImage(img);
    XCloseDisplay(dpy);
    printf("0x%06lx\n", pixel & 0xffffffUL);
    return 0;
}
