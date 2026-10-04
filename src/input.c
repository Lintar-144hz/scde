/*
 * SCDE - keyboard / mouse input, drag move & resize
 */
#include "scde.h"

#include <string.h>

static unsigned int mods_extra[4];

unsigned int strip_mods(unsigned int state)
{
    return state & ~(LockMask | sc.numlock_mask);
}

static unsigned int numlock_mask(void)
{
    XModifierKeymap *map;
    KeyCode numlock;
    unsigned int mask = 0;
    int i, j;

    numlock = XKeysymToKeycode(sc.dpy, XK_Num_Lock);
    map = XGetModifierMapping(sc.dpy);
    if (!map)
        return 0;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < map->max_keypermod; j++) {
            if (map->modifiermap[i * map->max_keypermod + j] == numlock) {
                mask = 1u << i;
                i = 8;
                break;
            }
        }
    }
    XFreeModifiermap(map);
    return mask;
}

static void grab_key(KeySym sym, unsigned int modifiers)
{
    KeyCode code = XKeysymToKeycode(sc.dpy, sym);
    size_t i;

    if (code == 0)
        return;
    for (i = 0; i < sizeof(mods_extra) / sizeof(mods_extra[0]); i++)
        XGrabKey(sc.dpy, code, modifiers | mods_extra[i], sc.root, True,
                 GrabModeAsync, GrabModeAsync);
}

void input_grab_keys(void)
{
    sc.numlock_mask = numlock_mask();
    mods_extra[0] = 0;
    mods_extra[1] = LockMask;
    mods_extra[2] = sc.numlock_mask;
    mods_extra[3] = sc.numlock_mask | LockMask;

    grab_key(XK_F4, Mod1Mask);              /* close */
    grab_key(XK_F2, Mod1Mask);              /* run command */
    grab_key(XK_space, Mod1Mask);           /* application menu */
    grab_key(XK_Return, Mod1Mask);          /* terminal */
    grab_key(XK_Tab, Mod1Mask);             /* switch windows */
    grab_key(XK_Tab, Mod1Mask | ShiftMask);
    grab_key(XK_ISO_Left_Tab, Mod1Mask | ShiftMask);
    grab_key(XK_Up, Mod1Mask);              /* maximize */
    grab_key(XK_Down, Mod1Mask);            /* minimize */
    grab_key(XK_Left, Mod1Mask);            /* snap left */
    grab_key(XK_Right, Mod1Mask);           /* snap right */
    grab_key(XK_d, Mod1Mask);               /* show desktop */
    grab_key(XK_e, Mod1Mask);               /* file manager / browser */

    XSync(sc.dpy, False);
}

static void grab_button(unsigned int button, unsigned int modifiers, Cursor cur)
{
    size_t i;

    for (i = 0; i < sizeof(mods_extra) / sizeof(mods_extra[0]); i++)
        XGrabButton(sc.dpy, button, modifiers | mods_extra[i], sc.root, False,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, cur);
}

void input_grab_buttons(void)
{
    grab_button(Button1, Mod1Mask, sc.cur_move);
    grab_button(Button3, Mod1Mask, sc.cur_resize);
    XSync(sc.dpy, False);
}

/* --------------------------------------------------------------- dragging */

void start_drag(Client *c, XButtonEvent *ev, int mode, int dirflags)
{
    XEvent e, last;
    int px = ev->x_root;
    int py = ev->y_root;
    int fx0 = c->x, fy0 = c->y, w0 = c->w, h0 = c->h;
    int dir = dirflags & DRAG_DIR_MASK;
    Cursor cur = (mode == DRAG_MOVE) ? sc.cur_move : sc.cur_resize;

    if (XGrabPointer(sc.dpy, sc.root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cur,
                     ev->time) != GrabSuccess)
        return;

    memset(&last, 0, sizeof(last));

    for (;;) {
        XNextEvent(sc.dpy, &e);

        if (e.type == MotionNotify) {
            int dx, dy, nw, nh, nx, ny;

            while (XCheckMaskEvent(sc.dpy, PointerMotionMask, &last))
                e = last;

            dx = e.xmotion.x_root - px;
            dy = e.xmotion.y_root - py;
            if (mode == DRAG_MOVE) {
                c->x = fx0 + dx;
                c->y = fy0 + dy;
                {
                    int fh = frame_h(c);
                    if (c->x + c->w < 64)
                        c->x = 64 - c->w;
                    if (c->x > sc.sw - 64)
                        c->x = sc.sw - 64;
                    if (c->y < 0)
                        c->y = 0;
                    if (c->y + fh > sc.sh)
                        c->y = sc.sh - fh;
                    if (c->y > sc.sh - 32)
                        c->y = sc.sh - 32;
                }
                XMoveWindow(sc.dpy, c->frame, c->x, c->y);
            } else {
                int ph = panel_reserved();
                int max_bottom = sc.sh - ph;
                int top_limit = cfg.panel_top ? ph : 0;

                nw = w0;
                nh = h0;
                nx = fx0;
                ny = fy0;

                if ((dir & DIR_LEFT) && !(dir & DIR_RIGHT)) {
                    nw = w0 - dx;
                    if (nw < MIN_CLIENT_SIZE)
                        nw = MIN_CLIENT_SIZE;
                    nx = fx0 + (w0 - nw);
                } else if (dir & (DIR_RIGHT | DIR_LEFT)) {
                    nw = w0 + dx;          /* free edge or whole-window drag */
                }
                if ((dir & DIR_UP) && !(dir & DIR_DOWN)) {
                    nh = h0 - dy;
                    if (nh < MIN_CLIENT_SIZE)
                        nh = MIN_CLIENT_SIZE;
                    ny = fy0 + (h0 - nh);
                } else if (dir & (DIR_DOWN | DIR_UP)) {
                    nh = h0 + dy;
                }
                if (nw < MIN_CLIENT_SIZE)
                    nw = MIN_CLIENT_SIZE;
                if (nh < MIN_CLIENT_SIZE)
                    nh = MIN_CLIENT_SIZE;
                if (nx < -(nw - 64))
                    nx = -(nw - 64);
                if (nx > sc.sw - 64)
                    nx = sc.sw - 64;
                if (ny < top_limit)
                    ny = top_limit;
                if (ny + c->title_h + nh + 2 * c->bw > max_bottom)
                    nh = max_bottom - ny - c->title_h - 2 * c->bw;
                if (nh < MIN_CLIENT_SIZE) {
                    nh = MIN_CLIENT_SIZE;
                    ny = max_bottom - c->title_h - 2 * c->bw - nh;
                    if (ny < top_limit)
                        ny = top_limit;
                }
                if (c->maximized)
                    c->maximized = 0;

                c->x = nx;
                c->y = ny;
                c->w = nw;
                c->h = nh;
                frame_sync(c);
            }
        } else if (e.type == ButtonRelease) {
            break;
        } else {
            if (e.type == MapRequest)
                handle_map_request(&e.xmaprequest);
            else if (e.type == ConfigureRequest)
                handle_configure_request(&e.xconfigurerequest);
            else if (e.type == DestroyNotify)
                handle_destroy_notify(&e.xdestroywindow);
            else if (e.type == UnmapNotify)
                handle_unmap_notify(&e.xunmap);
            else if (e.type == EnterNotify)
                handle_enter_notify(&e.xcrossing);
            else if (e.type == KeyPress)
                handle_key_press(&e.xkey);
            else if (e.type == Expose)
                handle_expose(&e.xexpose);
            else if (e.type == MotionNotify &&
                     e.xmotion.window == sc.panel)
                panel_motion(&e.xmotion);
            else if (e.type == MotionNotify && e.xmotion.window == sc.menu)
                menu_motion(&e.xmotion);
            else if (e.type == PropertyNotify)
                handle_property_notify(&e.xproperty);
            else if (e.type == ClientMessage)
                handle_client_message(&e.xclient);
            else if (e.type == ButtonPress)
                handle_button_press(&e.xbutton);
        }

        if (client_find(c->win) == NULL)
            break;                          /* window vanished mid-drag */
    }

    XUngrabPointer(sc.dpy, CurrentTime);
    if (client_find(c->win)) {
        frame_paint(c);
        panel_update();
    }
    XFlush(sc.dpy);
}

/* ------------------------------------------------------------------ keys */

void handle_key_press(XKeyEvent *ev)
{
    KeySym sym = XLookupKeysym(ev, 0);
    unsigned int mod = strip_mods(ev->state);
    unsigned int base = mod & ~ShiftMask;
    Client *c;

    if (msgbox_key(ev))
        return;
    if (fm_open() && ev->window == fm_window()) {
        fm_key(ev);
        return;
    }
    if (menu_is_open()) {
        menu_key(ev);
        return;
    }
    if (cmd_is_open()) {
        cmd_key(ev);
        return;
    }
    if (base != Mod1Mask)
        return;

    switch (sym) {
    case XK_F4:
        close_client(sc.focused);
        break;
    case XK_F2:
        cmd_show();
        break;
    case XK_space:
        if (menu_is_open())
            menu_hide();
        else
            menu_show();
        break;
    case XK_Return:
        spawn(cfg.terminal);
        break;
    case XK_Tab:
        cycle_windows((mod & ShiftMask) ? -1 : 1);
        break;
    case XK_ISO_Left_Tab:
        cycle_windows(-1);
        break;
    case XK_Up:
        c = sc.focused;
        if (c)
            client_maximize(c, !c->maximized);
        break;
    case XK_Down:
        c = sc.focused;
        if (c)
            client_minimize(c, c->minimized ? 0 : 1);
        break;
    case XK_Left:
        if (sc.focused)
            snap_client(sc.focused, 0);
        break;
    case XK_Right:
        if (sc.focused)
            snap_client(sc.focused, 1);
        break;
    case XK_d:
        show_desktop_toggle();
        break;
    case XK_e:
        fm_show();
        break;
    default:
        break;
    }
}

void handle_button_press(XButtonEvent *ev)
{
    Client *c = NULL;
    Window root_ret = None;

    if (msgbox_button(ev))
        return;
    if (fm_button(ev))
        return;
    Window child = None;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;

    if (ev->window == sc.panel) {
        panel_button_press(ev);
        return;
    }
    if (menu_is_open() && ev->window == sc.menu) {
        menu_pointer(ev);
        return;
    }
    if (cmd_is_open() && ev->window == sc.cmd) {
        cmd_hide();
        return;
    }
    if (ev->window == sc.root) {
        unsigned int m = strip_mods(ev->state);

        if (m & Mod1Mask) {
            /* Alt+click on the desktop background: move/resize nothing */
            if (!XQueryPointer(sc.dpy, sc.root, &root_ret, &child, &root_x,
                               &root_y, &win_x, &win_y, &mask))
                return;
            if (child == None)
                return;
            c = client_find(child);
            if (!c)
                c = client_from_frame(child);
            if (!c)
                return;
            if (ev->button == Button1)
                start_drag(c, ev, DRAG_MOVE, 0);
            else if (ev->button == Button3)
                start_drag(c, ev, DRAG_RESIZE,
                           DIR_UP | DIR_DOWN | DIR_LEFT | DIR_RIGHT);
            return;
        }
        if (ev->button == Button3) {
            desktop_menu_show(ev);           /* right click on the desktop */
            return;
        }
        return;
    }

    /* frame windows (title bar, buttons, edges) */
    if (client_from_frame(ev->window)) {
        handle_frame_button_press(ev);
        return;
    }
}

void handle_mapping_notify(XMappingEvent *ev)
{
    XRefreshKeyboardMapping(ev);
    if (ev->request == MappingKeyboard || ev->request == MappingModifier)
        input_grab_keys();
}
