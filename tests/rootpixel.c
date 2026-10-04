/*
 * Prints the root window pixel at (0,0) - used to verify the desktop
 * background colour set by SCDE.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>

int main(void)
{
    Display *dpy;
    Window root;
    XImage *img;
    unsigned long pixel;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "rootpixel: cannot open display\n");
        return 2;
    }
    root = RootWindow(dpy, DefaultScreen(dpy));
    img = XGetImage(dpy, root, 0, 0, 1, 1, AllPlanes, ZPixmap);
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
