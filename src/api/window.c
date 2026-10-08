/*
 * SCDE App API - top-level window creation and interaction.
 *
 * The API keeps its own Display connection and a fixed pool of windows, so
 * it never touches the window manager's state and never allocates.
 */
#include <scde/event.h>
#include <scde/window.h>

#include "internal.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <string.h>

#define WIN_POOL 8
#define WIN_TITLE_LEN 128

struct scde_window {
    int used;
    Window id;
    int x, y;
    unsigned int width, height;
    char title[WIN_TITLE_LEN];
};

static Display *api_dpy;
static struct scde_window win_pool[WIN_POOL];
static int handler_installed;
static XErrorHandler chained_handler;      /* NULL = Xlib default */

/* ------------------------------------------------------------- display -- */

static int api_xerror(Display *dpy, XErrorEvent *ev)
{
    char msg[128];

    if (chained_handler)
        return chained_handler(dpy, ev);
    msg[0] = '\0';
    XGetErrorText(dpy, ev->error_code, msg, sizeof(msg) - 1);
    scde_logf(SCDE_LOG_WARN, "window: X error: %s", msg);
    return 0;                            /* never abort the process */
}

int scde_api_open_display(void)
{
    if (api_dpy)
        return SCDE_OK;
    api_dpy = XOpenDisplay(NULL);
    if (!api_dpy)
        return SCDE_ERR_DISPLAY;
    if (!handler_installed) {
        /* keep whatever handler the host already installed (the window
           manager installs one) and only fall back to our own */
        chained_handler = XSetErrorHandler(api_xerror);
        handler_installed = 1;
    }
    return SCDE_OK;
}

void scde_api_close_display(void)
{
    if (!api_dpy)
        return;
    memset(win_pool, 0, sizeof(win_pool));   /* closing frees the windows */
    XCloseDisplay(api_dpy);
    api_dpy = NULL;
}

int scde_api_has_display(void)
{
    return api_dpy != NULL;
}

/* -------------------------------------------------------------- window -- */

static struct scde_window *find_slot(void)
{
    int i;

    for (i = 0; i < WIN_POOL; i++) {
        if (!win_pool[i].used)
            return &win_pool[i];
    }
    return NULL;
}

static struct scde_window *window_of(Window id)
{
    int i;

    if (id == None)
        return NULL;
    for (i = 0; i < WIN_POOL; i++) {
        if (win_pool[i].used && win_pool[i].id == id)
            return &win_pool[i];
    }
    return NULL;
}

static int check_win(const scde_window_t *w)
{
    if (!w || !w->used)
        return SCDE_ERR_ARG;
    if (!api_dpy)
        return SCDE_ERR_DISPLAY;
    return SCDE_OK;
}

int scde_window_create(const scde_window_attrs_t *attrs, scde_window_t **out)
{
    static const scde_window_attrs_t defaults = {
        SCDE_WINDOW_AUTO, SCDE_WINDOW_AUTO, 640, 400,
        "SCDE application", 0, 0x1c1f26UL
    };
    const scde_window_attrs_t *a = attrs ? attrs : &defaults;
    XSetWindowAttributes wa;
    XWindowAttributes got;
    struct scde_window *w;
    unsigned long mask;
    int pos_x = 0;
    int pos_y = 0;
    int st;

    if (!out)
        return SCDE_ERR_ARG;
    *out = NULL;
    if (a->width == 0 || a->height == 0)
        return SCDE_ERR_ARG;

    st = scde_api_open_display();
    if (st != SCDE_OK)
        return st;

    w = find_slot();
    if (!w)
        return SCDE_ERR_FULL;

    memset(&wa, 0, sizeof(wa));
    wa.background_pixel = a->background ? a->background : 0x1c1f26UL;
    wa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                    ButtonPressMask | ButtonReleaseMask | StructureNotifyMask;
    mask = CWBackPixel | CWEventMask;
    if (a->x != SCDE_WINDOW_AUTO) {
        pos_x = a->x;
        mask |= CWX;
    }
    if (a->y != SCDE_WINDOW_AUTO) {
        pos_y = a->y;
        mask |= CWY;
    }

    w->id = XCreateWindow(api_dpy, DefaultRootWindow(api_dpy), pos_x, pos_y,
                          a->width, a->height, 0, CopyFromParent, InputOutput,
                          CopyFromParent, mask, &wa);
    if (w->id == None)
        return SCDE_ERR;

    w->used = 1;
    w->width = a->width;
    w->height = a->height;
    if (XGetWindowAttributes(api_dpy, w->id, &got)) {
        w->x = got.x;
        w->y = got.y;
        w->width = (unsigned int)got.width;
        w->height = (unsigned int)got.height;
    } else {
        w->x = (a->x == SCDE_WINDOW_AUTO) ? 0 : a->x;
        w->y = (a->y == SCDE_WINDOW_AUTO) ? 0 : a->y;
    }

    if (!a->override_redirect) {
        Atom wm_protocols = XInternAtom(api_dpy, "WM_PROTOCOLS", False);
        Atom wm_delete = XInternAtom(api_dpy, "WM_DELETE_WINDOW", False);

        XSetWMProtocols(api_dpy, w->id, &wm_delete, 1);
        (void)wm_protocols;
    }

    scde_window_set_title(w, a->title ? a->title : "SCDE application");
    XFlush(api_dpy);

    *out = w;
    return SCDE_OK;
}

void scde_window_destroy(scde_window_t *w)
{
    if (!w || !w->used)
        return;
    if (api_dpy) {
        XUnmapWindow(api_dpy, w->id);
        XDestroyWindow(api_dpy, w->id);
        XFlush(api_dpy);
    }
    memset(w, 0, sizeof(*w));
}

int scde_window_show(scde_window_t *w)
{
    int st = check_win(w);

    if (st != SCDE_OK)
        return st;
    XMapRaised(api_dpy, w->id);
    XFlush(api_dpy);
    return SCDE_OK;
}

int scde_window_hide(scde_window_t *w)
{
    int st = check_win(w);

    if (st != SCDE_OK)
        return st;
    XUnmapWindow(api_dpy, w->id);
    XFlush(api_dpy);
    return SCDE_OK;
}

int scde_window_set_title(scde_window_t *w, const char *title)
{
    int st = check_win(w);

    if (st != SCDE_OK)
        return st;
    if (!title)
        return SCDE_ERR_ARG;
    snprintf(w->title, sizeof(w->title), "%s", title);
    XStoreName(api_dpy, w->id, w->title);
    XFlush(api_dpy);
    return SCDE_OK;
}

int scde_window_set_size(scde_window_t *w, unsigned int width,
                         unsigned int height)
{
    int st = check_win(w);

    if (st != SCDE_OK)
        return st;
    if (width == 0 || height == 0)
        return SCDE_ERR_ARG;
    XResizeWindow(api_dpy, w->id, width, height);
    w->width = width;
    w->height = height;
    XFlush(api_dpy);
    return SCDE_OK;
}

int scde_window_set_background(scde_window_t *w, unsigned long color)
{
    int st = check_win(w);

    if (st != SCDE_OK)
        return st;
    XSetWindowBackground(api_dpy, w->id, color);
    XClearWindow(api_dpy, w->id);
    XFlush(api_dpy);
    return SCDE_OK;
}

unsigned long scde_window_id(const scde_window_t *w)
{
    if (!w || !w->used)
        return 0;
    return (unsigned long)w->id;
}

/* ------------------------------------------------------- event mapping -- */

static int translate(XEvent *xev, scde_window_t *target, scde_event_t *out)
{
    memset(out, 0, sizeof(*out));
    out->window = target;
    out->time_ms = scde_event_now_ms();

    switch (xev->type) {
    case Expose:
        if (xev->xexpose.count != 0)
            return 0;                       /* only the last one */
        out->type = SCDE_EVENT_WINDOW_EXPOSE;
        out->width = xev->xexpose.width;
        out->height = xev->xexpose.height;
        out->code = (uint32_t)xev->xexpose.count;
        return 1;
    case ConfigureNotify:
        out->type = SCDE_EVENT_WINDOW_CONFIGURE;
        out->x = xev->xconfigure.x;
        out->y = xev->xconfigure.y;
        out->width = xev->xconfigure.width;
        out->height = xev->xconfigure.height;
        return 1;
    case KeyPress:
    case KeyRelease:
        out->type = SCDE_EVENT_WINDOW_KEY;
        out->code = (uint32_t)XLookupKeysym(&xev->xkey, 0);
        out->state = xev->xkey.state;
        out->x = xev->xkey.x;
        out->y = xev->xkey.y;
        return 1;
    case ButtonPress:
    case ButtonRelease:
        out->type = SCDE_EVENT_WINDOW_BUTTON;
        out->code = xev->xbutton.button;
        out->state = xev->xbutton.state;
        out->x = xev->xbutton.x;
        out->y = xev->xbutton.y;
        return 1;
    case ClientMessage:
        if ((Atom)xev->xclient.data.l[0] !=
            XInternAtom(api_dpy, "WM_DELETE_WINDOW", False))
            return 0;
        out->type = SCDE_EVENT_WINDOW_CLOSE;
        snprintf(out->text, sizeof(out->text), "wm-delete");
        return 1;
    default:
        return 0;
    }
}

int scde_window_process_events(scde_window_t *w, int max_events)
{
    int posted = 0;

    if (!w || !w->used)
        return SCDE_ERR_ARG;
    if (!api_dpy)
        return SCDE_ERR_DISPLAY;
    if (max_events <= 0)
        max_events = 64;

    while (posted < max_events && XPending(api_dpy) > 0) {
        XEvent xev;
        scde_event_t ev;
        scde_window_t *target;

        XNextEvent(api_dpy, &xev);
        target = window_of(xev.xany.window);
        if (!target)
            continue;
        if (translate(&xev, target, &ev) == 0)
            continue;
        if (scde_event_post(&ev) == SCDE_OK)
            posted++;
    }
    return posted;
}
