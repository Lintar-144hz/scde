/*
 * SCDE - launcher (searchable app menu), desktop menu, Alt+F2 run prompt
 */
#include "scde.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_PAD       6
#define MENU_MAX_W     400
#define MENU_MIN_W     200
#define MENU_MAX_ROWS  18
#define CMD_W          520
#define SEP_LABEL      "-"

/* searchable launcher (Alt+Space) layout */
#define LAUNCH_W       384       /* compact, fixed width */
#define LAUNCH_HEAD    36        /* search field row */
#define LAUNCH_SEP     1         /* rule under the field */
#define LAUNCH_TEXT_X  40        /* shared text column (icon + field) */
#define LAUNCH_ICON_X  12
#define LAUNCH_ROW_X   6
#define FIELD_H        24        /* height of every text field */
#define FIELD_PAD      10        /* field inset from the window edge */
#define ROW_MIN_H      26
#define LAUNCH_MIN_ROWS 1        /* shrink to the number of results */

/* pinned entries + separator + scanned .desktop files */
#define MENU_FILTER_MAX (MAX_MENU_ITEMS + 2 + 512)

/* --------------------------------------------------------- menu state ---- */

static int menu_open;
static int menu_sel;
static int menu_scroll;
static int menu_mx, menu_my, menu_mw, menu_mh, menu_item_h, menu_rows;
static MenuEntry *menu_src;
static int menu_n;
static int menu_desktop;            /* 1 = desktop / icon context menu */
static int menu_search;             /* 1 = app launcher with search field */
static int menu_icon;               /* icon tile size (launcher only) */
static char menu_query[96];
static int menu_filter[MENU_FILTER_MAX];   /* visible row -> menu_src */
static int menu_vis;                       /* visible row count */
static int menu_anchor_x = -1, menu_anchor_y = -1;  /* fixed while open */
static MenuEntry desktop_items[24];
static int desktop_n;

static void menu_geometry(void);
static void menu_create_window(void);
static void menu_draw(void);

Window menu_window(void)
{
    return sc.menu;
}

int menu_is_open(void)
{
    return menu_open;
}

/* ------------------------------------------------------------- helpers --- */

static int is_sep(const MenuEntry *e)
{
    return e->cmd[0] == '\0' ||
           (strcmp(e->label, SEP_LABEL) == 0 && e->cmd[0] == '\0');
}

/* blend c1 toward c2 by pct percent (0..100) */
static unsigned long mix_color(unsigned long c1, unsigned long c2, int pct)
{
    unsigned int r1 = (unsigned int)((c1 >> 16) & 0xff);
    unsigned int g1 = (unsigned int)((c1 >> 8) & 0xff);
    unsigned int b1 = (unsigned int)(c1 & 0xff);
    unsigned int r2 = (unsigned int)((c2 >> 16) & 0xff);
    unsigned int g2 = (unsigned int)((c2 >> 8) & 0xff);
    unsigned int b2 = (unsigned int)(c2 & 0xff);
    unsigned int r = (r1 * (unsigned int)(100 - pct) + r2 * (unsigned int)pct) / 100;
    unsigned int g = (g1 * (unsigned int)(100 - pct) + g2 * (unsigned int)pct) / 100;
    unsigned int b = (b1 * (unsigned int)(100 - pct) + b2 * (unsigned int)pct) / 100;

    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
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

/* visible row i -> source entry; i must be inside 0..menu_vis-1 */
static MenuEntry *vis_entry(int i)
{
    return &menu_src[menu_filter[i]];
}

static int vis_sep(int i)
{
    if (i < 0 || i >= menu_vis)
        return 1;
    return is_sep(&menu_src[menu_filter[i]]);
}

/* case-insensitive substring match */
static int match_query(const char *label, const char *q)
{
    const char *p;
    size_t qlen;

    if (!q || *q == '\0')
        return 1;
    if (!label)
        return 0;
    qlen = strlen(q);
    for (p = label; *p; p++) {
        size_t i;

        for (i = 0; i < qlen; i++) {
            if (p[i] == '\0')
                break;
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)q[i]))
                break;
        }
        if (i == qlen)
            return 1;
    }
    return 0;
}

/* muted, desaturated tile colours - calm, never neon */
static unsigned long tile_color(const char *name)
{
    static const unsigned long pal[] = {
        0x46617aUL, 0x4a7264UL, 0x635e7eUL, 0x74674eUL,
        0x556b78UL, 0x6f5f5fUL, 0x566e63UL
    };
    unsigned int h = 0;

    for (; *name; name++)
        h = h * 31u + (unsigned int)(unsigned char)*name;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* first letter of the label, for the icon tile */
static void tile_glyph(const char *label, char *out, size_t out_size)
{
    const char *p;

    if (out_size == 0)
        return;
    out[0] = '\0';
    if (!label || *label == '\0')
        return;
    for (p = label; *p; p++) {
        if (isalnum((unsigned char)*p))
            break;
    }
    if (*p == '\0') {
        out[0] = '?';
        out[1] = '\0';
        return;
    }
    out[0] = (char)toupper((unsigned char)*p);
    out[1] = '\0';
}

/* ---------------------------------------------------------- navigation --- */

static void menu_skip(int dir)
{
    int i = menu_sel;

    if (menu_vis <= 0)
        return;
    for (;;) {
        i += dir;
        if (i < 0)
            i = menu_vis - 1;
        if (i >= menu_vis)
            i = 0;
        if (!vis_sep(i) || i == menu_sel)
            break;
    }
    menu_sel = i;
}

static void menu_clamp_view(void)
{
    if (menu_vis <= 0) {
        menu_sel = 0;
        menu_scroll = 0;
        return;
    }
    if (menu_sel >= menu_vis)
        menu_sel = menu_vis - 1;
    if (menu_sel < 0)
        menu_sel = 0;
    if (menu_sel < menu_scroll)
        menu_scroll = menu_sel;
    if (menu_sel > menu_scroll + menu_rows - 1)
        menu_scroll = menu_sel - menu_rows + 1;
    if (menu_scroll > menu_vis - menu_rows)
        menu_scroll = menu_vis - menu_rows;
    if (menu_scroll < 0)
        menu_scroll = 0;
}

/* rebuild the visible list from the current query */
static void menu_refilter(void)
{
    int i;

    menu_vis = 0;
    for (i = 0; i < menu_n && menu_vis < MENU_FILTER_MAX; i++) {
        if (is_sep(&menu_src[i]) && menu_query[0] != '\0')
            continue;                       /* separators only when idle */
        if (!match_query(menu_src[i].label, menu_query))
            continue;
        menu_filter[menu_vis++] = i;
    }
    if (menu_sel >= menu_vis)
        menu_sel = menu_vis > 0 ? menu_vis - 1 : 0;
    if (menu_sel < 0)
        menu_sel = 0;
}

/* geometry + selection after a filter change, keeping the window put */
static void menu_layout(void)
{
    menu_geometry();
    if (vis_sep(menu_sel))
        menu_skip(1);
    menu_clamp_view();
}

static void menu_relayout(void)
{
    if (!menu_open)
        return;
    menu_refilter();
    menu_layout();
    menu_create_window();
    menu_draw();
}

/* y of the first result row inside the window */
static int row_top(void)
{
    return menu_search ? (LAUNCH_HEAD + LAUNCH_SEP) : MENU_PAD;
}

/* ----------------------------------------------------------- geometry --- */

static void menu_geometry(void)
{
    int i, maxw = 0, rows;

    if (menu_search) {
        int avail, maxr;

        menu_icon = sc.font_height + 5;
        if (menu_icon < 18)
            menu_icon = 18;
        menu_item_h = sc.font_height + 12;
        if (menu_item_h < menu_icon + 8)
            menu_item_h = menu_icon + 8;
        if (menu_item_h < ROW_MIN_H)
            menu_item_h = ROW_MIN_H;

        menu_mw = LAUNCH_W;
        if (menu_mw > sc.sw - 8)
            menu_mw = sc.sw - 8;
        if (menu_mw < 160)
            menu_mw = 160;

        avail = sc.sh - cfg.panel_height - 24 -
                (LAUNCH_HEAD + LAUNCH_SEP);
        maxr = avail / menu_item_h;
        if (maxr < LAUNCH_MIN_ROWS)
            maxr = LAUNCH_MIN_ROWS;

        rows = menu_vis;
        if (rows < LAUNCH_MIN_ROWS)
            rows = LAUNCH_MIN_ROWS;
        if (rows > maxr)
            rows = maxr;
        menu_rows = rows;
        menu_mh = LAUNCH_HEAD + LAUNCH_SEP + rows * menu_item_h;

        menu_mx = (sc.sw - menu_mw) / 2;
        menu_my = cfg.panel_top ? cfg.panel_height + 12
                                : sc.sh - cfg.panel_height - menu_mh - 12;
        if (menu_my < 0)
            menu_my = 0;
        if (menu_mx < 0)
            menu_mx = 0;
        /* keep the field still while the list shrinks or grows */
        if (menu_anchor_y >= 0) {
            menu_mx = menu_anchor_x;
            menu_my = menu_anchor_y;
            if (menu_my + menu_mh > sc.sh - cfg.panel_height - 4)
                menu_my = sc.sh - cfg.panel_height - menu_mh - 4;
            if (menu_my < 0)
                menu_my = 0;
        }
        return;
    }

    for (i = 0; i < menu_vis; i++) {
        int w;

        if (vis_sep(i))
            continue;
        w = text_width(vis_entry(i)->label) + 44;
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

    rows = menu_vis;
    if (rows > MENU_MAX_ROWS)
        rows = MENU_MAX_ROWS;
    if (menu_vis > 0 && rows > sc.sh / (menu_item_h * 2))
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
    menu_search = !desktop;
    menu_src = src;
    menu_n = n;
    if (!menu_src || menu_n <= 0)
        return;

    menu_query[0] = '\0';
    menu_sel = 0;
    menu_scroll = 0;
    menu_anchor_x = menu_anchor_y = -1;
    menu_refilter();
    menu_layout();
    if (menu_search) {
        menu_anchor_x = menu_mx;
        menu_anchor_y = menu_my;
    }
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
    ADD("File Manager", "@files");
    ADD("Web Browser", "@browser");
    ADD("-", "");
    ADD("Run command...", "@run");
    ADD("Applications", "@apps");
    ADD("-", "");
    ADD("New Folder", "@d-newdir");
    ADD("New File", "@d-newfile");
    ADD("Refresh", "@d-refresh");
    ADD("Security Center", "@security");
    ADD("-", "");
    ADD("Log out", "@logout");
#undef ADD

    menu_mx = ev ? ev->x_root : 4;
    menu_my = ev ? ev->y_root : 4;
    menu_open_common(1, desktop_items, desktop_n);
}

/* anchor an arbitrary item list at the given pointer position */
void menu_show_custom(MenuEntry *items, int n, int x, int y)
{
    menu_mx = x;
    menu_my = y;
    menu_open_common(1, items, n);
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

/* ------------------------------------------------------------ drawing --- */

/* inset text field: quiet background, hairline border, optional lens */
static void draw_field(Window w, int x, int y, int fw, int fh, int lens)
{
    unsigned long bg = mix_color(cfg.col_menu_bg, 0xffffffUL, 7);
    int cx = x + 12, cy = y + fh / 2;

    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, w, sc.gc, x, y, (unsigned)fw, (unsigned)fh);
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, w, sc.gc, x, y, (unsigned)(fw - 1),
                   (unsigned)(fh - 1));
    if (!lens)
        return;
    XSetForeground(sc.dpy, sc.gc,
                   mix_color(cfg.col_menu_fg, cfg.col_menu_bg, 45));
    XDrawArc(sc.dpy, w, sc.gc, cx - 5, cy - 5, 9, 9, 0, 360 * 64);
    XDrawLine(sc.dpy, w, sc.gc, cx + 3, cy + 3, cx + 7, cy + 7);
}

/* classic menu rows: separator, selection, run chevron */
static void menu_draw_plain(void)
{
    int row, bl;

    for (row = 0; row < menu_rows; row++) {
        int y0 = MENU_PAD + row * menu_item_h;
        int i = menu_scroll + row;
        MenuEntry *e;
        unsigned long fg;

        if (i >= menu_vis)
            break;
        e = vis_entry(i);
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
}

/* launcher body: search field, hairline rule, icon + label results */
static void menu_draw_launch(void)
{
    unsigned long dim = mix_color(cfg.col_menu_fg, cfg.col_menu_bg, 45);
    unsigned long bright = mix_color(cfg.col_menu_fg, 0xffffffUL, 30);
    int top = row_top();
    int row, bl, fy = (LAUNCH_HEAD - FIELD_H) / 2;

    draw_field(sc.menu, FIELD_PAD, fy, menu_mw - 2 * FIELD_PAD, FIELD_H, 1);
    bl = fy + (FIELD_H + sc.font_ascent - sc.font_descent) / 2;
    if (menu_query[0] != '\0') {
        int cx;

        draw_text_clip(sc.menu, LAUNCH_TEXT_X, menu_mw - FIELD_PAD - 6, bl,
                       bright, menu_query);
        cx = LAUNCH_TEXT_X + text_width(menu_query) + 3;
        if (cx > menu_mw - FIELD_PAD - 4)
            cx = menu_mw - FIELD_PAD - 4;
        XSetForeground(sc.dpy, sc.gc, bright);
        XFillRectangle(sc.dpy, sc.menu, sc.gc, cx, bl - sc.font_ascent, 1,
                       (unsigned)(sc.font_ascent + sc.font_descent));
    } else {
        draw_text(sc.menu, LAUNCH_TEXT_X, bl, dim, "Type to search...");
    }

    XSetForeground(sc.dpy, sc.gc,
                   mix_color(cfg.col_menu_bg, cfg.col_menu_fg, 22));
    XDrawLine(sc.dpy, sc.menu, sc.gc, 0, LAUNCH_HEAD, menu_mw, LAUNCH_HEAD);

    if (menu_vis == 0) {
        bl = top + (menu_item_h + sc.font_ascent - sc.font_descent) / 2;
        draw_text(sc.menu, LAUNCH_TEXT_X, bl, dim, "No matches");
        return;
    }

    for (row = 0; row < menu_rows; row++) {
        int y0 = top + row * menu_item_h;
        int i = menu_scroll + row;
        MenuEntry *e;
        unsigned long fg;

        if (i >= menu_vis)
            break;
        e = vis_entry(i);
        bl = y0 + (menu_item_h + sc.font_ascent - sc.font_descent) / 2;

        if (is_sep(e)) {
            XSetForeground(sc.dpy, sc.gc,
                           mix_color(cfg.col_menu_bg, cfg.col_menu_fg, 18));
            XDrawLine(sc.dpy, sc.menu, sc.gc, LAUNCH_ROW_X,
                      y0 + menu_item_h / 2, menu_mw - LAUNCH_ROW_X,
                      y0 + menu_item_h / 2);
            continue;
        }

        if (i == menu_sel) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, sc.menu, sc.gc, LAUNCH_ROW_X, y0 + 1,
                           (unsigned)(menu_mw - 2 * LAUNCH_ROW_X),
                           (unsigned)(menu_item_h - 2));
        }

        {
            char g[2];
            int iy = y0 + (menu_item_h - menu_icon) / 2;

            tile_glyph(e->label, g, sizeof(g));
            XSetForeground(sc.dpy, sc.gc, tile_color(e->label));
            XFillRectangle(sc.dpy, sc.menu, sc.gc, LAUNCH_ICON_X, iy,
                           (unsigned)menu_icon, (unsigned)menu_icon);
            if (g[0] != '\0') {
                int gw = text_width(g);
                int gbl = iy + (menu_icon + sc.font_ascent -
                                sc.font_descent) / 2;

                draw_text(sc.menu, LAUNCH_ICON_X + (menu_icon - gw) / 2, gbl,
                          mix_color(cfg.col_menu_fg, 0xffffffUL, 45), g);
            }
        }

        fg = (i == menu_sel) ? bright : cfg.col_menu_fg;
        draw_text_clip(sc.menu, LAUNCH_TEXT_X, menu_mw - 16, bl, fg,
                       e->label);
    }
}

static void menu_draw(void)
{
    if (!sc.menu || !menu_open)
        return;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sc.menu, sc.gc, 0, 0, (unsigned)menu_mw,
                   (unsigned)menu_mh);

    if (menu_search)
        menu_draw_launch();
    else
        menu_draw_plain();

    if (menu_search) {
        /* quiet scrollbar thumb when the list overflows */
        if (menu_vis > menu_rows) {
            int track = menu_mh - row_top() - 6;
            int th = track * menu_rows / menu_vis;
            int ty;

            if (th < 12)
                th = 12;
            ty = row_top() + 3 +
                 (track - th) * menu_scroll / (menu_vis - menu_rows);
            XSetForeground(sc.dpy, sc.gc,
                           mix_color(cfg.col_menu_bg, cfg.col_menu_fg, 30));
            XFillRectangle(sc.dpy, sc.menu, sc.gc, menu_mw - 5, ty, 3,
                           (unsigned)th);
        }
    } else if (menu_scroll > 0 || menu_scroll + menu_rows < menu_vis) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_panel_fg);
        if (menu_scroll > 0)
            XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 8, 2, menu_mw - 4, 6);
        if (menu_scroll + menu_rows < menu_vis)
            XDrawLine(sc.dpy, sc.menu, sc.gc, menu_mw - 8, menu_mh - 6,
                      menu_mw - 4, menu_mh - 2);
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sc.menu, sc.gc, 0, 0, (unsigned)(menu_mw - 1),
                   (unsigned)(menu_mh - 1));
}

/* ------------------------------------------------------------ actions --- */

static void menu_activate(int index)
{
    MenuEntry e;

    if (index < 0 || index >= menu_vis)
        return;
    e = *vis_entry(index);
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
        else if (strcmp(e.cmd, "@security") == 0)
            security_gui_toggle();
        else if (strcmp(e.cmd, "@logout") == 0)
            sc.running = 0;
        else
            desktop_command(e.cmd);     /* desktop / shortcut actions */
        return;
    }
    spawn(e.cmd);
}

void menu_key(XKeyEvent *ev)
{
    KeySym sym = XLookupKeysym(ev, 0);

    switch (sym) {
    case XK_Escape:
        menu_hide();
        return;
    case XK_Up:
    case XK_KP_Up:
        menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_Down:
    case XK_KP_Down:
        menu_skip(1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_Page_Up:
        menu_sel -= menu_rows;
        if (menu_sel < 0)
            menu_sel = 0;
        if (vis_sep(menu_sel))
            menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_Page_Down:
        menu_sel += menu_rows;
        if (menu_sel >= menu_vis)
            menu_sel = menu_vis - 1;
        if (vis_sep(menu_sel))
            menu_skip(1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_Home:
        menu_sel = 0;
        menu_skip(1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_End:
        menu_sel = menu_vis - 1;
        menu_skip(-1);
        menu_clamp_view();
        menu_draw();
        return;
    case XK_Return:
    case XK_KP_Enter:
        menu_activate(menu_sel);
        return;
    case XK_F2:
        menu_hide();
        cmd_show();
        return;
    default:
        break;
    }

    if (menu_search) {
        char buf[64];
        KeySym ks = NoSymbol;
        int len;

        if (sym == XK_BackSpace) {
            utf8_pop(menu_query);
            menu_relayout();
            return;
        }
        if (sym == XK_Delete ||
            (sym == XK_u && (ev->state & ControlMask))) {
            if (menu_query[0] != '\0') {
                menu_query[0] = '\0';
                menu_relayout();
            }
            return;
        }
        len = XLookupString(ev, buf, sizeof(buf) - 1, &ks, NULL);
        (void)ks;
        if (len > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f &&
            strlen(menu_query) + (size_t)len < sizeof(menu_query) - 1 &&
            !(ev->state & (Mod1Mask | Mod4Mask))) {
            strncat(menu_query, buf,
                    sizeof(menu_query) - strlen(menu_query) - 1);
            menu_relayout();
        }
        return;
    }

    /* plain menus keep their legacy keys */
    if (sym == XK_space)
        menu_activate(menu_sel);
}

/* pointer events are delivered to the grab window even outside the menu */
void menu_pointer(XButtonEvent *ev)
{
    int inside;
    int top;
    int idx;

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
    if (!inside) {
        menu_hide();
        return;
    }

    top = row_top();
    if (ev->y_root < menu_my + top) {
        /* search field / top padding */
        if (menu_search)
            return;
        menu_hide();
        return;
    }
    idx = (ev->y_root - menu_my - top) / menu_item_h;
    if (idx >= 0 && idx < menu_rows && menu_scroll + idx < menu_vis)
        menu_activate(menu_scroll + idx);
    else
        menu_hide();
}

void menu_motion(XMotionEvent *ev)
{
    int row, idx;
    int bottom = menu_search ? menu_mh : menu_mh - MENU_PAD;

    if (!menu_open)
        return;
    if (ev->y_root < menu_my + row_top())
        return;
    if (ev->y_root >= menu_my + bottom)
        return;
    row = (ev->y_root - menu_my - row_top()) / menu_item_h;
    idx = menu_scroll + row;
    if (row < 0 || row >= menu_rows || idx < 0 || idx >= menu_vis)
        return;
    if (idx == menu_sel)
        return;
    if (vis_sep(idx))
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
    cmd_h = sc.font_height + 24;
    if (cmd_h < 40)
        cmd_h = 40;
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
                               (unsigned)cmd_w, (unsigned)cmd_h, 1,
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
    unsigned long dim = mix_color(cfg.col_menu_fg, cfg.col_menu_bg, 45);
    unsigned long bright = mix_color(cfg.col_menu_fg, 0xffffffUL, 30);
    int fy = 8;
    int fh = cmd_h - 16;
    int bl;

    if (!sc.cmd || !cmd_open)
        return;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sc.cmd, sc.gc, 0, 0, (unsigned)cmd_w,
                   (unsigned)cmd_h);

    draw_field(sc.cmd, FIELD_PAD, fy, cmd_w - 2 * FIELD_PAD, fh, 0);
    bl = fy + (fh + sc.font_ascent - sc.font_descent) / 2;

    /* quiet chevron prompt, same column as the launcher text */
    XSetForeground(sc.dpy, sc.gc, dim);
    XDrawLine(sc.dpy, sc.cmd, sc.gc, LAUNCH_TEXT_X - 18, bl - 5,
              LAUNCH_TEXT_X - 14, bl);
    XDrawLine(sc.dpy, sc.cmd, sc.gc, LAUNCH_TEXT_X - 14, bl,
              LAUNCH_TEXT_X - 18, bl + 5);

    if (cmd_buf[0] != '\0') {
        int cx = LAUNCH_TEXT_X + text_width(cmd_buf) + 3;

        draw_text_clip(sc.cmd, LAUNCH_TEXT_X, cmd_w - FIELD_PAD - 6, bl,
                       bright, cmd_buf);
        if (cx > cmd_w - FIELD_PAD - 4)
            cx = cmd_w - FIELD_PAD - 4;
        XSetForeground(sc.dpy, sc.gc, bright);
        XFillRectangle(sc.dpy, sc.cmd, sc.gc, cx, bl - sc.font_ascent, 1,
                       (unsigned)(sc.font_ascent + sc.font_descent));
    } else {
        draw_text(sc.cmd, LAUNCH_TEXT_X, bl, dim, "Run a command or URL...");
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sc.cmd, sc.gc, 0, 0, (unsigned)(cmd_w - 1),
                   (unsigned)(cmd_h - 1));
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
