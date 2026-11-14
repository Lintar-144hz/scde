/*
 * SCDE - window management (reparented frames with a calm Harbor title bar)
 */
#include "scde.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- basics */

Client *client_find(Window w)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->win == w)
            return c;
    return NULL;
}

Client *client_from_frame(Window frame)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->frame == frame)
            return c;
    return NULL;
}

int client_count(void)
{
    Client *c;
    int n = 0;

    for (c = sc.clients; c; c = c->next)
        n++;
    return n;
}

int frame_w(Client *c)
{
    return c->w;
}

int frame_h(Client *c)
{
    return c->title_h + c->h;
}

static void set_wm_state(Window w, long state)
{
    unsigned long data[2];

    data[0] = (unsigned long)state;
    data[1] = 0;
    XChangeProperty(sc.dpy, w, sc.atom_wm_state, sc.atom_wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

void frame_sync(Client *c)
{
    if (c->frame == None)
        return;
    if (c->fullscreen) {
        XSetWindowBorderWidth(sc.dpy, c->frame, 0);
        XMoveResizeWindow(sc.dpy, c->frame, 0, 0, sc.sw, sc.sh);
        XMoveResizeWindow(sc.dpy, c->win, 0, 0, sc.sw, sc.sh);
        return;
    }
    XSetWindowBorderWidth(sc.dpy, c->frame, (unsigned)c->bw);
    XMoveResizeWindow(sc.dpy, c->frame, c->x, c->y, (unsigned)c->w,
                      (unsigned)frame_h(c));
    XMoveResizeWindow(sc.dpy, c->win, 0, c->title_h, (unsigned)c->w,
                      (unsigned)c->h);
    ui_shape_round(c->frame, c->w, frame_h(c), 11);
}

/* ---------------------------------------------------------- title bar art */

#define TRAFFIC_D 13
#define TRAFFIC_GAP 8
#define TRAFFIC_LEFT 14

static int traffic_span(void)
{
    return 3 * TRAFFIC_D + 2 * TRAFFIC_GAP;
}

void frame_paint(Client *c)
{
    int W, th, bl;
    unsigned long bg, top, bottom, fg;
    int hover = -1;

    if (c->frame == None || c->fullscreen || c->minimized)
        return;

    W = c->w;
    th = c->title_h;
    if (sc.focused == c) {
        bg = cfg.col_title_bg;
        top = ui_mix(bg, 0xffffffUL, 8);
        bottom = ui_mix(bg, 0x000000UL, 7);
        fg = cfg.col_title_fg;
    } else {
        bg = cfg.col_title_bg_inactive;
        top = ui_mix(bg, 0xffffffUL, 3);
        bottom = ui_mix(bg, 0x000000UL, 5);
        fg = cfg.col_title_fg_inactive;
    }

    ui_round_fill_gradient(c->frame, 0, 0, W, th, 0, top, bottom);
    XSetForeground(sc.dpy, sc.gc, ui_mix(top, 0xffffffUL, 8));
    XDrawLine(sc.dpy, c->frame, sc.gc, 0, 0, W, 0);
    XSetForeground(sc.dpy, sc.gc, ui_mix(bottom, 0x000000UL, 38));
    XDrawLine(sc.dpy, c->frame, sc.gc, 0, th - 1, W, th - 1);

    if (c->hover_btn == BTN_CLOSE)
        hover = UI_LIGHT_CLOSE;
    else if (c->hover_btn == BTN_MIN)
        hover = UI_LIGHT_MIN;
    else if (c->hover_btn == BTN_MAX)
        hover = UI_LIGHT_MAX;

    if (W > traffic_span() + 80) {
        ui_draw_traffic(c->frame, TRAFFIC_LEFT, th / 2,
                        sc.focused == c, hover, c->maximized, bg);
    }

    bl = (th + sc.font_ascent - sc.font_descent) / 2;
    {
        int right = W - 14;
        int left = traffic_span() + 30;
        int tw;
        char shown[NAME_MAX_LEN];

        if (W > traffic_span() + 120) {
            tw = text_width_bold(c->name);
            if (tw <= right - left) {
                draw_text_bold(c->frame, (W - tw) / 2, bl, fg, c->name);
            } else {
                str_copy(shown, sizeof(shown), c->name);
                draw_text_clip_font(c->frame, sc.font_bold, left, right, bl,
                                    fg, shown);
            }
        } else if (right - left > 24) {
            draw_text_clip_font(c->frame, sc.font_bold, left, right, bl, fg,
                                c->name);
        }
    }
}

int frame_button_at(Client *c, int x, int y)
{
    int cell = TRAFFIC_D + TRAFFIC_GAP;
    int left = TRAFFIC_LEFT - 6;

    if (y < 0 || y >= c->title_h)
        return BTN_NONE;
    if (x < left || x >= left + 3 * cell)
        return BTN_NONE;
    switch ((x - left) / cell) {
    case 0:  return BTN_CLOSE;
    case 1:  return BTN_MIN;
    default: return BTN_MAX;
    }
}

/* ----------------------------------------------------------------- focus */

void update_client_list(void)
{
    Window *wins;
    Client *c;
    int n = client_count();
    int i = 0;

    if (n == 0) {
        XDeleteProperty(sc.dpy, sc.root, sc.atom_net_client_list);
        return;
    }
    wins = calloc((size_t)n, sizeof(Window));
    if (!wins)
        return;
    for (c = sc.clients; c; c = c->next)
        wins[i++] = c->win;
    XChangeProperty(sc.dpy, sc.root, sc.atom_net_client_list, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)wins, n);
    free(wins);
}

/*
 * Publish the window that owns the X input focus.  Tracking a single
 * value (sc.focus_win) means the property can never disagree with the
 * focus we actually set, whether it is a managed client or one of our own
 * application windows.  (implementation lives in util.c next to the
 * focus helpers, so tests that link util.c alone stay linkable)
 */

void unfocus_all(void)
{
    Client *c;

    for (c = sc.clients; c; c = c->next)
        if (c->frame != None)
            XSetWindowBorder(sc.dpy, c->frame, cfg.col_border_unfocus);
    sc.focused = NULL;
}

void focus_client(Client *c)
{
    if (!c) {
        unfocus_all();
        update_active_property();
        panel_update();
        return;
    }
    if (c->minimized)
        client_minimize(c, 0);

    if (sc.focused == c) {
        if (c->frame != None)
            XSetWindowBorder(sc.dpy, c->frame, cfg.col_title_hover);
        frame_paint(c);
        return;
    }
    if (sc.focused && sc.focused != c) {
        if (sc.focused->frame != None)
            XSetWindowBorder(sc.dpy, sc.focused->frame,
                             cfg.col_border_unfocus);
        frame_paint(sc.focused);
    }

    sc.focused = c;
    if (c->frame != None)
        XSetWindowBorder(sc.dpy, c->frame, cfg.col_title_hover);
    frame_paint(c);
    focus_track(c->win);
    if (c->frame != None) {
        XRaiseWindow(sc.dpy, c->frame);
    }
    panel_raise();
    set_wm_state(c->win, NormalState);
    update_active_property();
    panel_update();
}

/* ---------------------------------------------------------- geometry ops */

static void clamp_to_desktop(Client *c)
{
    int top = reserve_top();
    int bot = reserve_bottom();
    int fh = frame_h(c);

    if (c->fullscreen)
        return;

    /* keep clear of the top menu bar and the floating dock */
    if (c->y < top)
        c->y = top;
    if (c->y + fh > sc.sh - bot)
        c->y = sc.sh - bot - fh;
    if (c->y < top)
        c->y = top;
    if (c->y > sc.sh - bot - 32)
        c->y = sc.sh - bot - 32;
    if (c->x + c->w < 64)
        c->x = 64 - c->w;
    if (c->x > sc.sw - 64)
        c->x = sc.sw - 64;
}

void client_maximize(Client *c, int on)
{
    int wy, wh;

    if (!c || c->fullscreen)
        return;
    if (on == c->maximized)
        return;

    if (on) {
        c->sx = c->x;
        c->sy = c->y;
        c->sw = c->w;
        c->sh = c->h;
        wy = reserve_top();
        wh = sc.sh - reserve_top() - reserve_bottom();
        c->x = 0;
        c->y = wy;
        c->w = sc.sw - 2 * c->bw;
        c->h = wh - c->title_h - 2 * c->bw;
        if (c->h < MIN_CLIENT_SIZE)
            c->h = MIN_CLIENT_SIZE;
        c->maximized = 1;
    } else {
        c->x = c->sx;
        c->y = c->sy;
        c->w = c->sw;
        c->h = c->sh;
        c->maximized = 0;
        clamp_to_desktop(c);
    }
    frame_sync(c);
    frame_paint(c);
    panel_update();
}

void client_minimize(Client *c, int on)
{
    if (!c || c->minimized == on)
        return;

    c->minimized = on;
    if (on) {
        Client *o;

        set_wm_state(c->win, IconicState);
        XUnmapWindow(sc.dpy, c->frame);
        if (sc.focused == c) {
            sc.focused = NULL;
            for (o = sc.clients; o; o = o->next)
                if (o != c && !o->minimized)
                    break;
            if (o)
                focus_client(o);
            else {
                /* nothing left to focus: drop the stale owner so the
                   active hint and the taskbar dot both go quiet */
                focus_drop(c->win);
                update_active_property();
            }
        }
        log_msg("minimize 0x%lx '%s'", (unsigned long)c->win, c->name);
    } else {
        set_wm_state(c->win, NormalState);
        XMapWindow(sc.dpy, c->frame);
        log_msg("restore 0x%lx '%s'", (unsigned long)c->win, c->name);
    }
    panel_update();
}

void show_desktop_toggle(void)
{
    Client *c;
    int visible = 0;
    /* internal windows hidden by show-desktop, restored on the way back */
    static int hid_fm, hid_sg;

    for (c = sc.clients; c; c = c->next)
        if (!c->minimized)
            visible++;
    if (fm_open() || security_gui_open())
        visible++;

    if (visible) {
        for (c = sc.clients; c; c = c->next)
            if (!c->minimized)
                client_minimize(c, 1);
        /* internal apps hide to their taskbar entry, like a minimise */
        hid_fm = fm_open();
        if (hid_fm)
            fm_hide();
        hid_sg = security_gui_open();
        if (hid_sg)
            security_gui_hide();
        sc.show_desktop = 1;
    } else {
        for (c = sc.clients; c; c = c->next)
            if (c->minimized)
                client_minimize(c, 0);
        if (hid_fm) {
            hid_fm = 0;
            fm_show();
        }
        if (hid_sg) {
            hid_sg = 0;
            security_gui_show();
        }
        sc.show_desktop = 0;
        if (sc.clients)
            focus_client(sc.clients);
    }
    panel_update();
}

void snap_client(Client *c, int side)
{
    int wy = reserve_top();
    int wh = sc.sh - reserve_top() - reserve_bottom();

    if (!c || c->fullscreen)
        return;
    if (c->maximized) {
        c->maximized = 0;
    }
    c->x = (side == 0) ? 0 : sc.sw / 2;
    c->y = wy;
    c->w = sc.sw / 2 - 2 * c->bw;
    c->h = wh - c->title_h - 2 * c->bw;
    if (c->h < MIN_CLIENT_SIZE)
        c->h = MIN_CLIENT_SIZE;
    frame_sync(c);
    frame_paint(c);
}

/* -------------------------------------------------------------- commands */

void close_client(Client *c)
{
    Atom *protocols = NULL;
    int n = 0, has_delete = 0, i;
    XEvent ev;

    if (!c)
        return;

    if (XGetWMProtocols(sc.dpy, c->win, &protocols, &n) && protocols) {
        for (i = 0; i < n; i++)
            if (protocols[i] == sc.atom_wm_delete)
                has_delete = 1;
        XFree(protocols);
    }

    if (has_delete) {
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c->win;
        ev.xclient.message_type = sc.atom_wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)sc.atom_wm_delete;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(sc.dpy, c->win, False, NoEventMask, &ev);
        log_msg("close (WM_DELETE_WINDOW) 0x%lx", (unsigned long)c->win);
    } else {
        XKillClient(sc.dpy, c->win);
        log_msg("close (XKillClient) 0x%lx", (unsigned long)c->win);
    }
}

void cycle_windows(int dir)
{
    Client *c, *prev = NULL, *picked = NULL;

    if (!sc.clients)
        return;

    if (!sc.focused) {
        picked = (dir >= 0) ? sc.clients : NULL;
        if (!picked) {
            for (c = sc.clients; c; c = c->next)
                if (!c->next)
                    picked = c;
        }
        focus_client(picked);
        return;
    }

    if (dir >= 0) {
        picked = sc.focused->next ? sc.focused->next : sc.clients;
    } else {
        for (c = sc.clients; c && c != sc.focused; c = c->next)
            prev = c;
        picked = prev;
        if (!picked) {
            for (c = sc.clients; c; c = c->next)
                if (!c->next)
                    picked = c;
        }
    }
    focus_client(picked);
}

/*
 * The X server's real input focus - one round trip, only when an action
 * targets "the active window" (Alt+F4, Alt+Down, the menu bar).
 */
static Window current_focus(void)
{
    Window f = None;
    int revert = RevertToNone;

    if (sc.dpy)
        XGetInputFocus(sc.dpy, &f, &revert);
    return f;
}

/* close the focused window, internal applications included */
void active_close(void)
{
    Window fw = current_focus();

    if (fm_open() && (fw == fm_window() || sc.focus_win == fm_window())) {
        fm_close();
        return;
    }
    if (security_gui_open() &&
        (fw == security_gui_window() ||
         sc.focus_win == security_gui_window())) {
        security_gui_close();
        return;
    }
    close_client(sc.focused);
}

/* minimise the focused window (or restore it), internal apps included */
void active_minimize(void)
{
    Window fw = current_focus();

    if (fm_open() && (fw == fm_window() || sc.focus_win == fm_window())) {
        fm_hide();
        return;
    }
    if (security_gui_open() &&
        (fw == security_gui_window() ||
         sc.focus_win == security_gui_window())) {
        security_gui_hide();
        return;
    }
    if (sc.focused)
        client_minimize(sc.focused, sc.focused->minimized ? 0 : 1);
}

/* ----------------------------------------------------------------- manage */

static int window_is_fullscreen(Window w)
{
    Atom actual;
    int fmt;
    unsigned long nitems = 0, bytes = 0;
    unsigned char *data = NULL;
    int found = 0;
    int i;

    if (XGetWindowProperty(sc.dpy, w, sc.atom_net_wm_state, 0, 64, False,
                           sc.atom_net_wm_state, &actual, &fmt, &nitems,
                           &bytes, &data) == Success && data) {
        if (actual == sc.atom_net_wm_state && fmt == 32) {
            Atom *atoms = (Atom *)data;
            for (i = 0; i < (int)nitems; i++)
                if (atoms[i] == sc.atom_net_wm_state_fullscreen)
                    found = 1;
        }
        XFree(data);
    }
    return found;
}

static void fetch_name(Client *c)
{
    char *name = NULL;

    str_copy(c->name, sizeof(c->name), "untitled");
    if (XFetchName(sc.dpy, c->win, &name) && name) {
        str_copy(c->name, sizeof(c->name), name);
        XFree(name);
    }
}

static void create_frame(Client *c, int fx, int fy)
{
    XSetWindowAttributes wa;

    wa.override_redirect = True;
    wa.background_pixel = cfg.col_title_bg_inactive;
    wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                    EnterWindowMask | ExposureMask;

    c->frame = XCreateWindow(sc.dpy, sc.root, fx, fy, (unsigned)c->w,
                             (unsigned)frame_h(c), (unsigned)c->bw,
                             sc.depth, InputOutput, sc.visual,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &wa);
    XSetWindowBorder(sc.dpy, c->frame, cfg.col_border_unfocus);
}

void manage(Window w)
{
    XWindowAttributes wa;
    Client *c, *tail;
    int fx, fy;

    if (w == None || w == sc.panel || w == sc.dock || w == sc.menu ||
        w == sc.cmd || w == sc.wm_check)
        return;
    if (client_find(w))
        return;
    if (!XGetWindowAttributes(sc.dpy, w, &wa))
        return;
    if (wa.override_redirect)
        return;

    c = calloc(1, sizeof(Client));
    if (!c)
        return;

    c->win = w;
    c->frame = None;
    c->w = wa.width > 0 ? wa.width : 320;
    c->h = wa.height > 0 ? wa.height : 240;
    c->bw = cfg.border_width;
    c->title_h = cfg.title_height;
    c->fullscreen = window_is_fullscreen(w);
    fetch_name(c);

    fx = wa.x - c->bw;
    fy = wa.y - c->bw - c->title_h;
    if (fy < 0)
        fy = 0;
    c->x = fx;
    c->y = fy;

    XSelectInput(sc.dpy, w, EnterWindowMask | PropertyChangeMask |
                           StructureNotifyMask);
    XSetWindowBorderWidth(sc.dpy, w, 0);
    XAddToSaveSet(sc.dpy, w);
    input_grab_client(w);

    create_frame(c, c->x, c->y);
    if (wa.map_state == IsViewable)
        c->ignore_unmap += 2;           /* reparent: unmap to old parent + self */
    XReparentWindow(sc.dpy, w, c->frame, 0, c->title_h);

    clamp_to_desktop(c);

    if (!sc.clients) {
        sc.clients = c;
    } else {
        for (tail = sc.clients; tail->next; tail = tail->next)
            ;
        tail->next = c;
    }

    set_wm_state(w, NormalState);
    XMapWindow(sc.dpy, w);
    frame_sync(c);
    XMapWindow(sc.dpy, c->frame);

    update_client_list();
    focus_client(c);
    log_msg("manage 0x%lx '%s' %dx%d frame+%d+%d", (unsigned long)w, c->name,
            c->w, c->h, c->x, c->y);
}

void unmanage(Client *c, int client_alive)
{
    Client *prev = NULL, *it;

    if (!c)
        return;
    log_msg("unmanage 0x%lx '%s'", (unsigned long)c->win, c->name);

    for (it = sc.clients; it && it != c; it = it->next)
        prev = it;
    if (it == c) {
        if (prev)
            prev->next = c->next;
        else
            sc.clients = c->next;
    }

    if (sc.focused == c)
        sc.focused = NULL;
    focus_drop(c->win);

    if (client_alive) {
        XSelectInput(sc.dpy, c->win, NoEventMask);
        input_ungrab_client(c->win);
        XUnmapWindow(sc.dpy, c->frame);
        XReparentWindow(sc.dpy, c->win, sc.root, c->x + c->bw,
                        c->y + c->bw + c->title_h);
        XRemoveFromSaveSet(sc.dpy, c->win);
        XSetWindowBorderWidth(sc.dpy, c->win, (unsigned)cfg.border_width);
    }
    if (c->frame != None) {
        XDestroyWindow(sc.dpy, c->frame);
        c->frame = None;
    }

    free(c);

    update_client_list();
    /* when nothing holds the focus any more, fall back to a managed client;
       if one of our own application windows still has it, leave it alone */
    if (!sc.focused && sc.clients && sc.focus_win == None) {
        /* skip hidden windows: restoring a minimized one would pop it up */
        for (it = sc.clients; it; it = it->next)
            if (!it->minimized)
                break;
        focus_client(it);
    } else
        update_active_property();
    panel_update();
}

/* ----------------------------------------------------------------- events */

void handle_map_request(XMapRequestEvent *ev)
{
    manage(ev->window);
}

void handle_configure_request(XConfigureRequestEvent *ev)
{
    Client *c = client_find(ev->window);
    XWindowChanges wc;
    unsigned int mask = (unsigned int)ev->value_mask;
    int top = reserve_top();
    int bottom = reserve_bottom();

    if (!c) {
        /* window is not ours (or has not been managed yet) */
        wc.x = ev->x;
        wc.y = ev->y;
        wc.width = ev->width;
        wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;
        XConfigureWindow(sc.dpy, ev->window, mask, &wc);
        return;
    }

    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    mask &= ~(unsigned int)CWBorderWidth;
    if (c->fullscreen) {
        mask &= (unsigned int)(CWSibling | CWStackMode);
        XConfigureWindow(sc.dpy, c->frame, mask, &wc);
        return;
    }

    if (mask & CWWidth)
        c->w = wc.width < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : wc.width;
    if (mask & CWHeight)
        c->h = wc.height < MIN_CLIENT_SIZE ? MIN_CLIENT_SIZE : wc.height;
    if (mask & CWX) {
        int nx = wc.x - c->bw;
        if (nx < 0)
            nx = 0;
        if (nx > sc.sw - 32)
            nx = sc.sw - 32;
        c->x = nx;
    }
    if (mask & CWY) {
        int ny = wc.y - c->bw - c->title_h;

        if (ny < top)
            ny = top;
        if (ny > sc.sh - bottom - 32)
            ny = sc.sh - bottom - 32;
        c->y = ny;
    }

    frame_sync(c);
    frame_paint(c);

    if (mask & CWStackMode) {
        wc.sibling = None;
        XConfigureWindow(sc.dpy, c->frame,
                         mask & (unsigned int)(CWSibling | CWStackMode), &wc);
        panel_raise();                  /* keep the panel on top */
    }
    panel_update();
}

void handle_destroy_notify(XDestroyWindowEvent *ev)
{
    Client *c = client_find(ev->window);

    if (c)
        unmanage(c, 0);
}

void handle_unmap_notify(XUnmapEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c)
        return;
    if (c->ignore_unmap > 0) {
        c->ignore_unmap--;
        return;
    }
    if (c->minimized)
        return;                         /* we unmapped the frame ourselves */
    unmanage(c, 1);
}

void handle_enter_notify(XCrossingEvent *ev)
{
    Client *c;

    if (ev->mode != NotifyNormal)
        return;
    if (menu_is_open() || cmd_is_open())
        return;
    c = client_find(ev->window);
    if (!c)
        c = client_from_frame(ev->window);
    if (!c)
        return;
    if (c->hover_btn != BTN_NONE) {     /* stale hover from the last visit */
        c->hover_btn = BTN_NONE;
        frame_paint(c);
    }
    if (cfg.focus_follows_mouse)
        focus_client(c);
}

void handle_property_notify(XPropertyEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c)
        return;
    if (ev->atom == XA_WM_NAME || ev->atom == sc.atom_net_wm_name) {
        fetch_name(c);
        frame_paint(c);
        panel_update();
    }
}

void handle_client_message(XClientMessageEvent *ev)
{
    Client *c = client_find(ev->window);

    if (!c) {
        /* our own application windows answer _NET_ACTIVE_WINDOW as well,
           so tools (xdotool windowactivate) can raise/focus them */
        if (ev->message_type == sc.atom_net_active) {
            if (ev->window == fm_window() && fm_window() != None) {
                fm_show();
            } else if (ev->window == security_gui_window() &&
                       security_gui_window() != None) {
                security_gui_show();
            }
        }
        return;
    }
    if (ev->message_type == sc.atom_net_wm_state &&
        (Atom)ev->data.l[1] == sc.atom_net_wm_state_fullscreen) {
        long action = ev->data.l[0];
        int on = c->fullscreen;

        if (action == 0)
            on = 0;
        else if (action == 1)
            on = 1;
        else
            on = !on;
        if (on != c->fullscreen) {
            c->fullscreen = on;
            frame_sync(c);
            frame_paint(c);
            if (on)
                XRaiseWindow(sc.dpy, c->frame);
            panel_raise();
        }
    } else if (ev->message_type == sc.atom_net_active) {
        focus_client(c);
    }
}

void handle_expose(XExposeEvent *ev)
{
    Client *c;

    if (ev->window == sc.root) {         /* dragged windows reveal the */
        desktop_expose(ev);              /* desktop: repaint icons here */
        return;
    }
    if (msgbox_expose(ev))
        return;
    if (security_gui_expose(ev))
        return;
    if (fm_expose(ev))
        return;
    if (ev->count != 0)
        return;
    if ((c = client_from_frame(ev->window)) != NULL)
        frame_paint(c);
    else if (ev->window == sc.panel)
        panel_expose(ev);
    else if (ev->window == sc.dock)
        dock_expose(ev);
    else if (ev->window == sc.menu)
        menu_expose(ev);
    else if (ev->window == sc.cmd)
        cmd_expose(ev);
}

void handle_frame_motion(XMotionEvent *ev)
{
    Client *c = client_from_frame(ev->window);
    int b;

    if (!c || c->fullscreen)
        return;
    b = frame_button_at(c, ev->x, ev->y);
    if (b != c->hover_btn) {
        c->hover_btn = b;
        frame_paint(c);
    }
}

static Time last_click_time;
static Window last_click_win;

void handle_frame_button_press(XButtonEvent *ev)
{
    Client *c = client_from_frame(ev->window);
    int dir = 0;
    const int m = 5;

    if (!c)
        return;
    focus_client(c);
    if (c->minimized || c->fullscreen)
        return;

    if (strip_mods(ev->state) != 0)
        return;                         /* Alt+click is handled by root grabs */

    if (ev->y < c->title_h) {
        int b = frame_button_at(c, ev->x, ev->y);

        if (b == BTN_CLOSE) {
            close_client(c);
            return;
        }
        if (b == BTN_MAX) {
            client_maximize(c, !c->maximized);
            return;
        }
        if (b == BTN_MIN) {
            client_minimize(c, 1);
            return;
        }
        /* double click on the title bar toggles maximize */
        if (last_click_win == ev->window &&
            ev->time - last_click_time < 400) {
            last_click_win = None;
            client_maximize(c, !c->maximized);
            return;
        }
        last_click_win = ev->window;
        last_click_time = ev->time;
        start_drag(c, ev, DRAG_MOVE, 0);
        return;
    }

    /* edges resize */
    if (ev->x < 0 || (ev->x < m && ev->y >= c->title_h))
        dir |= DIR_LEFT;
    if (ev->x >= c->w || (ev->x >= c->w - m && ev->y >= c->title_h))
        dir |= DIR_RIGHT;
    if (ev->y < 0 || (ev->y >= c->title_h && ev->y < c->title_h + m))
        dir |= DIR_UP;
    if (ev->y >= c->title_h + c->h ||
        (ev->y >= c->title_h && ev->y >= c->title_h + c->h - m))
        dir |= DIR_DOWN;
    if (dir)
        start_drag(c, ev, DRAG_RESIZE, dir);
}

void manage_existing(void)
{
    Window root_ret, parent_ret, *children = NULL;
    unsigned int n = 0, i;

    if (!XQueryTree(sc.dpy, sc.root, &root_ret, &parent_ret, &children, &n))
        return;
    for (i = 0; i < n; i++) {
        XWindowAttributes wa;

        if (!XGetWindowAttributes(sc.dpy, children[i], &wa))
            continue;
        if (!wa.override_redirect) {
            if (wa.map_state != IsUnmapped)
                manage(children[i]);
            continue;
        }
        if (children[i] == sc.panel || children[i] == sc.dock ||
            children[i] == sc.menu || children[i] == sc.cmd ||
            children[i] == sc.wm_check)
            continue;

        /* stale frame left behind by a crashed instance: adopt its clients */
        {
            Window fr = children[i], fr_ret, fr_parent, *grand = NULL;
            unsigned int gn = 0, g;

            if (!XQueryTree(sc.dpy, fr, &fr_ret, &fr_parent, &grand, &gn))
                continue;
            for (g = 0; g < gn; g++) {
                XWindowAttributes gwa;

                if (!XGetWindowAttributes(sc.dpy, grand[g], &gwa))
                    continue;
                XReparentWindow(sc.dpy, grand[g], sc.root,
                                wa.x + wa.border_width,
                                wa.y + wa.border_width +
                                    (wa.height - gwa.height));
                XSetWindowBorderWidth(sc.dpy, grand[g],
                                      (unsigned)cfg.border_width);
                manage(grand[g]);
            }
            if (grand)
                XFree(grand);
            XDestroyWindow(sc.dpy, fr);
        }
    }
    if (children)
        XFree(children);
}
