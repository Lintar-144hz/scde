/*
 * Dumps the root window to a P6 PPM file:  screenshot <out.ppm>
 * (used by tests/shoot.sh for visual checks, no extra packages needed)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    Display *dpy;
    Window root;
    XImage *img;
    FILE *f;
    int w, h, x, y;
    unsigned long pixel;

    if (argc < 2) {
        fprintf(stderr, "usage: screenshot out.ppm\n");
        return 1;
    }
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "screenshot: cannot open display\n");
        return 2;
    }
    w = DisplayWidth(dpy, DefaultScreen(dpy));
    h = DisplayHeight(dpy, DefaultScreen(dpy));
    root = RootWindow(dpy, DefaultScreen(dpy));
    img = XGetImage(dpy, root, 0, 0, (unsigned)w, (unsigned)h, AllPlanes,
                    ZPixmap);
    if (!img) {
        fprintf(stderr, "screenshot: cannot read root window\n");
        XCloseDisplay(dpy);
        return 3;
    }
    f = fopen(argv[1], "wb");
    if (!f) {
        perror("screenshot");
        XDestroyImage(img);
        XCloseDisplay(dpy);
        return 4;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char rgb[3];
            pixel = XGetPixel(img, x, y);
            rgb[0] = (unsigned char)((pixel >> 16) & 0xff);
            rgb[1] = (unsigned char)((pixel >> 8) & 0xff);
            rgb[2] = (unsigned char)(pixel & 0xff);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    XDestroyImage(img);
    XCloseDisplay(dpy);
    return 0;
}
