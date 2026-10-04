/*
 * SCDE - launcher (Plasma style app menu), desktop menu, Alt+F2 run prompt
 */
#include "scde.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_PAD     6
#define MENU_MAX_W   400
#define MENU_MIN_W   200
#define MENU_MAX_ROWS 18
#define CMD_W        520
#define SEP_LABEL    "-"

/* --------------------------------------------------------- menu state ---- */

static int menu_open;
static int menu_sel;
static int menu_scroll;
static int menu_mx, menu_my, menu_mw, menu_mh, menu_item_h, menu_rows;
static MenuEntry *menu_src;
static int menu_n;
static int menu_desktop;            /* 1 = desktop right-click menu */
static MenuEntry desktop_items[8];
static int desktop_n;

Window menu_window(void)
{
    return sc.menu;
}

int menu_is_open(void)
{
    return menu_open;
}

static int is_sep(const MenuEntry *e)
{
    return e->cmd[0] == '\0' ||
           (strcmp(e->label, SEP_LABEL) == 0 && e->cmd[0] == '\0');
}

static void menu_skip(int dir)
{
    int i = menu_sel;

    for (;;) {
        i += dir;
        if (i < 0)
            i = menu_n - 1;
        if (i >= menu_n)
            i = 0;
        if (!is_sep(&menu_src[i]))
            break;
        if (i == menu_sel)
            break;
    }
    menu_sel = i;
}

static void menu_clamp_view(void)
{
    if (menu_sel < menu_scroll)
        menu_scroll = menu_sel;
    if (menu_sel > menu_scroll + menu_rows - 1)
        menu_scroll = menu_sel - menu_rows + 1;
    if (menu_scroll > menu_n - menu_rows)
        menu_scroll = menu_n - menu_rows;
    if (menu_scroll < 0)
        menu_scroll = 0;
}

static void menu_geometry(void)
{
    int i, maxw = 0, rows;

    for (i = 0; i < menu_n; i++) {
        int w;

        if (is_sep(&menu_src[i]))
            continue;
        w = text_width(menu_src[i].label) + 44;
        if (w > maxw)
            maxw = w;
    }
    menu_item_h = sc.font_height + 10;
    if (menu_item_h < 22)
        menu_item_h = 22;

    menu_mw = maxw;
    if (menu_mw < MENU_MIN_W)
        menu_mw = MENU_MIN_W;
    if (menu_mw > MENU_MAX_W)
        menu_mw = MENU_MAX_W;

    rows = menu_n;
    if (rows > MENU_MAX_ROWS)
        rows = MENU_MAX_ROWS;
    if (menu_n > 0 && rows > sc.sh / (menu_item_h * 2))
        rows = sc.sh / (menu_item_h * 2);
    if (rows < 1)
        rows = 1;
    menu_rows = rows;
    menu_mh = rows * menu_item_h + 2 * MENU_PAD;

    if (menu_desktop && menu_mx >= 0) {
        /* anchored at the pointer, flipped if it would leave the screen */
        if (menu_mx + menu_mw > sc.sw - 4)
            menu_mx = sc.sw - menu_mw - 4;
        if (menu_my + menu_mh > sc.sh - cfg.panel_height - 4)
            menu_my -= menu_mh + cfg.panel_height;
        if (menu_my < 4)
            menu_my = 4;
        if (menu_mx < 4)
            menu_mx = 4;
    } else {
        menu_mx = 4;
        if (cfg.panel_top)
            menu_my = cfg.panel_height + 4;
        else
            menu_my = sc.sh - cfg.panel_height - menu_mh - 4;
        if (menu_my < 0)
            menu_my = 0;
    }
}

static void menu_create_window(void)
{
    if (sc.menu == None) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_menu_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        EnterWindowMask;
        sc.menu = XCreateWindow(sc.dpy, sc.root, menu_mx, menu_my,
                                (unsigned)menu_mw, (unsigned)menu_mh, 1,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel | CWEventMask,
                                &wa);
        XStoreName(sc.dpy, sc.menu, "scde-menu");
    } else {
        XMoveResizeWindow(sc.dpy, sc.menu, menu_mx, menu_my,
                          (unsigned)menu_mw, (unsigned)menu_mh);
    }
}

static void menu_open_common(int desktop, MenuEntry *src, int n)
{
    if (menu_open)
        return;

    menu_desktop = desktop;
    menu_src = src;
    menu_n = n;
    if (!menu_src || menu_n <= 0)
        return;

    menu_geometry();
    menu_sel = 0;
    menu_scroll = 0;
    if (is_sep(&menu_src[0]))
        menu_skip(1);
    menu_clamp_view();
    menu_create_window();

    XMapRaised(sc.dpy, sc.menu);
    menu_open = 1;

    XGrabKeyboard(sc.dpy, sc.menu, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);
    XGrabPointer(sc.dpy, sc.menu, False,
                 ButtonPressMask | PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, sc.cur_normal,
                 CurrentTime);
    XFlush(sc.dpy);
}

void menu_show(void)
{
    menu_mx = 4;
    menu_my = 0;
    menu_open_common(0, sc.menu_items, sc.menu_item_count);
}

void desktop_menu_show(XButtonEvent *ev)
{
#define ADD(L, C) do { \
        str_copy(desktop_items[desktop_n].label, \
                 sizeof(desktop_items[0].label), (L)); \
        str_copy(desktop_items[desktop_n].cmd, \
                 sizeof(desktop_items[0].cmd), (C)); \
        desktop_n++; \
    } while (0)

    desktop_n = 0;
    ADD("Terminal", "@terminal");
    ADD("Editor", "@editor");
    ADD("Files", "@files");
    ADD("Web Browser", "@browser");
    ADD("Run command...", "@run");
    ADD("Applications", "@apps");
    ADD("-", "");
    ADD("Log out", "@logout");
#undef ADD

    menu_mx = ev ? ev->x_root : 4;
    menu_my = ev ? ev->y_root : 4;
    menu_open_common(1, desktop_items, desktop_n);
}

void menu_hide(void)
{
    if (!menu_open)
        return;
    menu_open = 0;
    XUngrabKeyboard(sc.dpy, CurrentTime);
    XUngrabPointer(sc.dpy, CurrentTime);
    if (sc.menu != None)
        XUnmapWindow(sc.dpy, sc.menu);
    XFlush(sc.dpy);
}

static void menu_draw(void)
{
    int i, row, bl;

    if (!sc.menu || !menu_open)
        return;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sc.menu, sc.gc, 0, 0, (unsigned)menu_mw,
                   (unsigned)menu_mh);

    for (row = 0; row < menu_rows; row++) {
        int y0 = MENU_PAD + row * menu_item_h;
        MenuEntry *e;
        unsigned long fg;

        i = menu_scroll + row;
        if (i >= menu_n)
            break;
        e = &menu_src[i];
        bl = y0 + (menu_item_h + sc.font_ascent - sc.font_descent) / 2;

        if (is_sep(e)) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
            XDrawLine(sc.dpy, sc.menu, sc.gc, 8, y0 + menu_item_h / 2,
                      menu_mw - 8, y0 + menu_item_h / 2);
            continue;
        }

        if (i == menu_sel) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, sc.menu, sc.gc, 2, y0,
                           (unsigned)(menu_mw - 4), (unsigned)menu_item_h);
        }
        fg = (i == menu_sel) ? 0xffffffUL : cfg.col_menu_fg;
        draw_text_clip(sc.menu, 16, menu_mw - 12, bl, fg, e->label);
        /* small "run" arrow on the right */
        XSetForeground(sc.dpy, sc.gc, fg);
        XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 16, bl - 5,
                  menu_mw - 12, bl);
        XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 12, bl,
                  menu_mw - 16, bl + 5);
    }

    /* scroll indicators */
    if (menu_scroll > 0 || menu_scroll + menu_rows < menu_n) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_panel_fg);
        if (menu_scroll > 0)
            XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 8, 2, menu_mw - 4, 6);
        if (menu_scroll + menu_rows < menu_n)
            XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 8, menu_mh - 6,
                      menu_mw - 4, menu_mh - 2);
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sc.menu, sc.gc, 0, 0, (unsigned)(menu_mw - 1),
                   (unsigned)(menu_mh - 1));
}

static void menu_activate(int index)
{
    MenuEntry e;
    int desktop_snapshot = menu_desktop;

    if (index < 0 || index >= menu_n)
        return;
    e = menu_src[index];
    if (is_sep(&e))
        return;
    menu_hide();

    if (e.cmd[0] == '@') {
        if (strcmp(e.cmd, "@run") == 0)
            cmd_show();
        else if (strcmp(e.cmd, "@terminal") == 0)
            apps_run_terminal("terminal");
        else if (strcmp(e.cmd, "@editor") == 0)
            apps_run_terminal("editor");
        else if (strcmp(e.cmd, "@files") == 0)
            fm_show();
        else if (strcmp(e.cmd, "@apps") == 0)
            menu_show();
        else if (strcmp(e.cmd, "@desktop") == 0)
            desktop_menu_show(NULL);
        else if (strcmp(e.cmd, "@browser") == 0)
            apps_run("browser", apps_browser());
        else if (strcmp(e.cmd, "@logout") == 0)
            sc.running = 0;
        return;
    }
    (void)desktop_snapshot;
    spawn(e.cmd);
}

void menu_key(XKeyEvent *ev)
{
    KeySym sym = XLookupKeysym(ev, 0);

    switch (sym) {
    case XK_Escape:
        menu_hide();
        break;
    case XK_Up:
    case XK_KP_Up:
        menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_Down:
    case XK_KP_Down:
        menu_skip(1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_Page_Up:
        menu_sel -= menu_rows;
        if (menu_sel < 0)
            menu_sel = 0;
        if (is_sep(&menu_src[menu_sel]))
            menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_Page_Down:
        menu_sel += menu_rows;
        if (menu_sel >= menu_n)
            menu_sel = menu_n - 1;
        if (is_sep(&menu_src[menu_sel]))
            menu_skip(1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_Home:
        menu_sel = 0;
        menu_skip(1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_End:
        menu_sel = menu_n - 1;
        menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        break;
    case XK_Return:
    case XK_KP_Enter:
    case XK_space:
        menu_activate(menu_sel);
        break;
    case XK_F2:
        menu_hide();
        cmd_show();
        break;
    default:
        break;
    }
}

/* pointer events are delivered to the grab window even outside the menu */
void menu_pointer(XButtonEvent *ev)
{
    int inside;
    int idx = -1;

    if (!menu_open)
        return;

    if (ev->button == Button4 || ev->button == Button5) {
        int dir = (ev->button == Button4) ? -1 : 1;

        menu_skip(dir);
        menu_clamp_view();
        menu_draw();
        return;
    }
    if (ev->button != Button1)
        return;

    inside = (ev->x_root >= menu_mx && ev->x_root < menu_mx + menu_mw &&
              ev->y_root >= menu_my && ev->y_root < menu_my + menu_mh);

    if (inside) {
        idx = (ev->y_root - menu_my - MENU_PAD) / menu_item_h;
        if (idx >= 0 && idx < menu_rows && menu_scroll + idx < menu_n)
            menu_activate(menu_scroll + idx);
        else
            menu_hide();
    } else {
        menu_hide();
    }
}

void menu_motion(XMotionEvent *ev)
{
    int row, idx;

    if (!menu_open)
        return;
    if (ev->y_root < menu_my + MENU_PAD ||
        ev->y_root >= menu_my + menu_mh - MENU_PAD)
        return;
    row = (ev->y_root - menu_my - MENU_PAD) / menu_item_h;
    idx = menu_scroll + row;
    if (row < 0 || row >= menu_rows || idx < 0 || idx >= menu_n)
        return;
    if (idx == menu_sel)
        return;
    if (is_sep(&menu_src[idx]))
        return;
    menu_sel = idx;
    menu_draw();
}

void menu_expose(XExposeEvent *ev)
{
    if (ev->count == 0)
        menu_draw();
}

/* --------------------------------------------------- command launcher --- */

static int cmd_open;
static char cmd_buf[256];
static int cmd_x, cmd_y, cmd_w, cmd_h;

Window cmd_window(void)
{
    return sc.cmd;
}

int cmd_is_open(void)
{
    return cmd_open;
}

void cmd_show(void)
{
    if (cmd_open)
        return;

    cmd_w = CMD_W;
    if (cmd_w > sc.sw - 20)
        cmd_w = sc.sw - 20;
    cmd_h = sc.font_height + 16;
    if (cmd_h < 34)
        cmd_h = 34;
    cmd_x = (sc.sw - cmd_w) / 2;
    cmd_y = cfg.panel_top ? cfg.panel_height + 18
                          : sc.sh - cfg.panel_height - cmd_h - 24;
    if (cmd_y < 0)
        cmd_y = 0;
    cmd_buf[0] = '\0';

    if (sc.cmd == None) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_menu_bg;
        wa.event_mask = ExposureMask | ButtonPressMask;
        sc.cmd = XCreateWindow(sc.dpy, sc.root, cmd_x, cmd_y,
                               (unsigned)cmd_w, (unsigned)cmd_h, 2,
                               CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, sc.cmd, "scde-cmd");
    } else {
        XMoveResizeWindow(sc.dpy, sc.cmd, cmd_x, cmd_y, cmd_w, cmd_h);
    }

    XMapRaised(sc.dpy, sc.cmd);
    cmd_open = 1;
    XGrabKeyboard(sc.dpy, sc.cmd, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);
    XFlush(sc.dpy);
}

void cmd_hide(void)
{
    if (!cmd_open)
        return;
    cmd_open = 0;
    XUngrabKeyboard(sc.dpy, CurrentTime);
    if (sc.cmd != None)
        XUnmapWindow(sc.dpy, sc.cmd);
    XFlush(sc.dpy);
}

static void cmd_draw(void)
{
    int bl;
    int x = 12;
    char prompt[] = "Run:";
    int pw;
    char shown[300];

    if (!sc.cmd || !cmd_open)
        return;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sc.cmd, sc.gc, 0, 0, (unsigned)cmd_w,
                   (unsigned)cmd_h);

    bl = (cmd_h + sc.font_ascent - sc.font_descent) / 2;
    pw = text_width(prompt) + 8;
    draw_text(sc.cmd, x, bl, cfg.col_border_focus, prompt);
    x += pw;

    snprintf(shown, sizeof(shown), "%s", cmd_buf);
    draw_text_clip(sc.cmd, x, cmd_w - 24, bl, 0xe8e8f0UL, shown);

    {
        int cw = text_width(cmd_buf);
        int cx = x + cw + 2;

        if (cx > cmd_w - 12)
            cx = cmd_w - 12;
        XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
        XFillRectangle(sc.dpy, sc.cmd, sc.gc, cx, bl - sc.font_ascent,
                       2, (unsigned)(sc.font_ascent + sc.font_descent));
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sc.cmd, sc.gc, 0, 0, (unsigned)(cmd_w - 1),
                   (unsigned)(cmd_h - 1));
}

static void utf8_pop(char *s)
{
    size_t len = strlen(s);

    if (len == 0)
        return;
    len--;
    while (len > 0 && (unsigned char)s[len] >= 0x80 &&
           (unsigned char)s[len] < 0xC0)
        len--;
    s[len] = '\0';
}

/* "example.com", "www.x.org", "https://host", "localhost:8080" -> a URL */
static int looks_like_url(const char *s)
{
    const char *p;

    if (!s || !*s)
        return 0;
    if (strstr(s, "://") != NULL)
        return 1;
    if (strncmp(s, "www.", 4) == 0)
        return 1;
    if (strchr(s, ' ') || strchr(s, '\t'))
        return 0;
    for (p = s; *p; p++) {
        if (isalpha((unsigned char)*p) || isdigit((unsigned char)*p))
            continue;
        if (strchr("./-_~?&=%#:@", *p))
            continue;
        return 0;
    }
    /* need at least one dot or a known scheme-less host shape */
    if (strchr(s, '.') == NULL && strcmp(s, "localhost") != 0)
        return 0;
    return 1;
}

static void cmd_submit(void)
{
    char run[400];

    if (cmd_buf[0] == '\0') {
        cmd_hide();
        return;
    }
    if (looks_like_url(cmd_buf)) {
        snprintf(run, sizeof(run), "%s '%s'", sc.browser, cmd_buf);
    } else {
        snprintf(run, sizeof(run), "%s", cmd_buf);
    }
    cmd_hide();
    spawn(run);
}

void cmd_key(XKeyEvent *ev)
{
    char buf[64];
    KeySym sym = NoSymbol;
    int len;

    len = XLookupString(ev, buf, sizeof(buf) - 1, &sym, NULL);
    buf[len > 0 ? len : 0] = '\0';

    if (sym == XK_Escape) {
        cmd_hide();
        return;
    }
    if (sym == XK_Return || sym == XK_KP_Enter) {
        cmd_submit();
        return;
    }
    if (sym == XK_BackSpace) {
        utf8_pop(cmd_buf);
        cmd_draw();
        return;
    }
    if (sym == XK_space && (ev->state & Mod1Mask)) {
        cmd_hide();
        menu_show();
        return;
    }
    if (len > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f &&
        strlen(cmd_buf) + (size_t)len < sizeof(cmd_buf) - 1 &&
        !(ev->state & Mod1Mask)) {
        strcat(cmd_buf, buf);
        cmd_draw();
        return;
    }
    /* ignore everything else while the prompt is open */
}

void cmd_expose(XExposeEvent *ev)
{
    if (ev->count == 0)
        cmd_draw();
}
