/*
 * SCDE - panel (Harbor style): launcher mark, task manager, tray, clock
 */
#include "scde.h"

#include <scde/security.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define LAUNCHER_W 44
#define TRAY_ICON_W 24
#define DESKTOP_W 8
#define BTN_MAX_W 176
#define BTN_MIN_W 44
#define BTN_CLOSE_W 16

typedef struct {
    int task;                 /* index into the taskbar registry */
    int x, w;
} PanelBtn;

static PanelBtn btns[MAX_PANEL_BTNS];
static int nbtns;
static char clock_str[128];
static char date_str[128];
static int hover_launcher;
static int hover_desktop;
static int hover_task = -1;
static int hover_task_close;
static int hover_tray = -1;
static int clock_has_seconds;
static int clock_w, tray_w, desk_w;
static int tray_draw, clock_draw;

/* 50% blend of two RRGGBB colours - used to dim the label of a minimized
   window so it reads as "not there" */
static unsigned long dim_color(unsigned long fg, unsigned long bg)
{
    return (((fg & 0xfefefeUL) >> 1) + ((bg & 0xfefefeUL) >> 1));
}

/* rounded chip: a plain rect with the corners cut back to the panel bg -
   the cheapest dock feel without a compositor (paint happens on the panel,
   so the untouched corners simply keep the panel colour) */
static void chip(Window w, int x, int y, int wd, int hg, int cut,
                 unsigned long fg)
{
    if (wd <= 2 * cut || hg <= 2 * cut || cut < 1)
        return;
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillRectangle(sc.dpy, w, sc.gc, x + cut, y, (unsigned)(wd - 2 * cut),
                   (unsigned)hg);
    XFillRectangle(sc.dpy, w, sc.gc, x, y + cut, (unsigned)wd,
                   (unsigned)(hg - 2 * cut));
}

int panel_reserved(void)
{
    return cfg.panel_height;
}

static int panel_y(void)
{
    return cfg.panel_top ? 0 : sc.sh - cfg.panel_height;
}

static int baseline(int y0)
{
    return y0 + (cfg.panel_height + sc.font_ascent - sc.font_descent) / 2;
}

static void format_now(char *buf, size_t size, const char *fmt)
{
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    if (strftime(buf, size, fmt, &tm) == 0)
        buf[0] = '\0';
}

static int two_line_clock(void)
{
    return cfg.panel_height >= 30 && cfg.clock_date_format[0] != '\0';
}

/* ------------------------------------------------------------------ icons */

static void icon_launcher(int y0)
{
    int cx = LAUNCHER_W / 2;
    int cy = y0 + cfg.panel_height / 2;
    int s = 7, g = 3, x0, y0i, i, j;

    x0 = cx - s - g / 2;
    y0i = cy - s - g / 2;
    /* Harbor mark: four calm tiles with a soft top edge, not flat neon */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            XFillRectangle(sc.dpy, sc.panel, sc.gc, x0 + i * (s + g),
                           y0i + j * (s + g), (unsigned)s, (unsigned)s);
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_fg);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            XFillRectangle(sc.dpy, sc.panel, sc.gc, x0 + i * (s + g),
                           y0i + j * (s + g), (unsigned)s, 1);
}

static void icon_network(int x, int cy, unsigned long fg, unsigned long bg)
{
    XPoint tri[3];

    (void)bg;
    tri[0].x = (short)(x + 5);
    tri[0].y = (short)(cy - 6);
    tri[1].x = (short)(x + 11);
    tri[1].y = (short)(cy - 6);
    tri[2].x = (short)(x + 8);
    tri[2].y = (short)(cy - 1);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, sc.panel, sc.gc, tri, 3, Convex, CoordModeOrigin);

    tri[0].x = (short)(x + 5);
    tri[0].y = (short)(cy + 2);
    tri[1].x = (short)(x + 11);
    tri[1].y = (short)(cy + 2);
    tri[2].x = (short)(x + 8);
    tri[2].y = (short)(cy + 7);
    XFillPolygon(sc.dpy, sc.panel, sc.gc, tri, 3, Convex, CoordModeOrigin);
}

static void icon_volume(int x, int cy, unsigned long fg)
{
    XPoint tri[3];

    tri[0].x = (short)(x + 4);
    tri[0].y = (short)(cy - 4);
    tri[1].x = (short)(x + 8);
    tri[1].y = (short)(cy - 4);
    tri[2].x = (short)(x + 8);
    tri[2].y = (short)(cy + 4);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, sc.panel, sc.gc, tri, 3, Convex, CoordModeOrigin);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, x + 8, cy - 4, 3, 8);
    XDrawArc(sc.dpy, sc.panel, sc.gc, x + 8, cy - 8, 10, 16,
             (300 << 6), (120 << 6));
}

static void icon_battery(int x, int cy, unsigned long fg, unsigned long bg)
{
    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawRectangle(sc.dpy, sc.panel, sc.gc, x + 3, cy - 6, 14, 11);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, x + 18, cy - 3, 2, 5);
    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, x + 5, cy - 4, 8, 7);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, x + 5, cy - 4, 5, 7);
}

/* Security Center: a filled shield, accent colour while protection runs */
static void icon_shield(int x, int cy, unsigned long fg)
{
    XPoint p[5];

    p[0].x = x;
    p[0].y = cy - 7;
    p[1].x = x + 14;
    p[1].y = cy - 7;
    p[2].x = x + 14;
    p[2].y = cy;
    p[3].x = x + 7;
    p[3].y = cy + 8;
    p[4].x = x;
    p[4].y = cy;
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, sc.panel, sc.gc, p, 5, Convex, CoordModeOrigin);
}

/* ----------------------------------------------------------------- panel */

void panel_create(void)
{
    XSetWindowAttributes wa;
    int y = panel_y();

    sc.panel_w = sc.sw;
    wa.override_redirect = True;
    wa.background_pixel = cfg.col_panel_bg;
    wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                    ButtonReleaseMask;

    sc.panel = XCreateWindow(sc.dpy, sc.root, 0, y, (unsigned)sc.panel_w,
                             (unsigned)cfg.panel_height, 0, CopyFromParent,
                             InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &wa);
    XStoreName(sc.dpy, sc.panel, "scde-panel");

    clock_has_seconds = strchr(cfg.clock_format, 'S') != NULL;
    format_now(clock_str, sizeof(clock_str), cfg.clock_format);
    format_now(date_str, sizeof(date_str), cfg.clock_date_format);

    XMapRaised(sc.dpy, sc.panel);
    panel_update();
}

void panel_destroy(void)
{
    if (sc.panel) {
        XDestroyWindow(sc.dpy, sc.panel);
        sc.panel = None;
    }
}

void panel_raise(void)
{
    if (sc.panel)
        XRaiseWindow(sc.dpy, sc.panel);
}

void panel_update(void)
{
    int y0, x, i, avail, n, w, shown, right;

    if (!sc.panel)
        return;

    /* single taskbar registry: managed clients first (mapping order),
       then the registered internal application windows */
    taskbar_sync();
    n = taskbar_count();

    y0 = 0;
    clock_str[0] = '\0';
    date_str[0] = '\0';
    if (cfg.show_clock) {
        format_now(clock_str, sizeof(clock_str), cfg.clock_format);
        format_now(date_str, sizeof(date_str), cfg.clock_date_format);
    }

    clock_w = cfg.show_clock && clock_str[0] ? text_width(clock_str) + 20 : 0;
    if (two_line_clock() && date_str[0]) {
        int dw = text_width(date_str) + 20;
        if (dw > clock_w)
            clock_w = dw;
    }
    tray_w = cfg.show_tray ? TRAY_ICON_W * 4 : 0;
    desk_w = cfg.show_desktop_btn ? DESKTOP_W : 0;

    /* drop the tray (and then the clock) instead of letting it run into the
       task buttons on a narrow screen */
    tray_draw = tray_w > 0 &&
                sc.sw - clock_w - tray_w - desk_w - 8 - (LAUNCHER_W + 6) >= 8;
    clock_draw = clock_w > 0 &&
                 sc.sw - clock_w - (tray_draw ? tray_w : 0) - desk_w - 8 -
                     (LAUNCHER_W + 6) >= 8;
    if (!clock_draw)
        tray_draw = 0;

    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, 0, 0, (unsigned)sc.panel_w,
                   (unsigned)cfg.panel_height);

    /* launcher: rounded dock chip on hover, quiet mark */
    if (hover_launcher) {
        chip(sc.panel, 3, y0 + 2, LAUNCHER_W - 6, cfg.panel_height - 4, 3,
             cfg.col_btn_hover);
    }
    icon_launcher(y0);
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XDrawRectangle(sc.dpy, sc.panel, sc.gc, LAUNCHER_W, 4, 0,
                   (unsigned)(cfg.panel_height - 8));

    x = LAUNCHER_W + 6;
    right = sc.sw - (clock_draw ? clock_w : 0) - (tray_draw ? tray_w : 0) -
            desk_w - 8;
    avail = right - x;
    if (avail < 0)
        avail = 0;

    shown = n;
    w = BTN_MAX_W;
    if (n > 0) {
        if (avail < 8) {
            shown = 0;                  /* no room for task buttons at all */
        } else {
            w = avail / n;
            if (w > BTN_MAX_W)
                w = BTN_MAX_W;
            if (w < BTN_MIN_W) {
                shown = avail / BTN_MIN_W;
                if (shown < 1)
                    shown = 1;
                if (shown > n)
                    shown = n;
                w = avail / shown;
                if (w > BTN_MAX_W)
                    w = BTN_MAX_W;
                if (w < 8)
                    shown = 0;
            }
        }
    }

    nbtns = 0;
    for (i = 0; i < shown; i++) {
        const TaskEntry *t = taskbar_at(i);
        char label[NAME_MAX_LEN];
        int active;
        unsigned long bg;
        unsigned long fg;
        unsigned long glyph_fg;
        int bl;

        if (!t)
            break;
        active = t->focused;

        if (x + 2 >= right)
            break;
        btns[nbtns].task = i;
        btns[nbtns].x = x;
        btns[nbtns].w = w;
        nbtns++;

        /* macOS dock: rounded tile until hovered / focused, plus a small
           running dot under the app mark (accent for the focused one) */
        bg = 0;
        if (active)
            bg = cfg.col_btn_active;
        else if (i == hover_task)
            bg = cfg.col_btn_hover;
        if (bg)
            chip(sc.panel, x + 1, y0 + 3, w - 5, cfg.panel_height - 6, 3,
                 bg);
        {
            unsigned long dc;

            if (active)
                dc = cfg.col_border_focus;
            else if (t->hidden)
                dc = dim_color(cfg.col_panel_fg, bg ? bg : cfg.col_panel_bg);
            else
                dc = cfg.col_panel_fg;
            XSetForeground(sc.dpy, sc.gc, dc);
            XFillArc(sc.dpy, sc.panel, sc.gc, x + 11,
                     y0 + cfg.panel_height - 9, 5, 5, 0, 360 * 64);
        }

        bl = baseline(y0);
        if (active)
            fg = 0xedf0f5UL;
        else if (t->hidden)
            fg = dim_color(cfg.col_panel_fg, bg ? bg : cfg.col_panel_bg);
        else
            fg = cfg.col_panel_fg;

        /* mini application mark left of the label: internal applications
           get their own calm tone so File Manager / SCDE Security read
           as separate apps, not duplicates of a client window */
        if (t->app_type == TASK_APP_FM)
            glyph_fg = 0xd9a84eUL;
        else if (t->app_type == TASK_APP_SECURITY)
            glyph_fg = 0x7c9cbfUL;
        else
            glyph_fg = fg;
        {
            int gy = y0 + (cfg.panel_height - 14) / 2;

            if (cfg.panel_height >= 26)
                gy -= 3;                 /* room for the running dot below */
            draw_win_glyph(sc.panel, x + 6, gy, 14, glyph_fg,
                           bg ? bg : cfg.col_panel_bg);
        }
        {
            int bi = nbtns - 1;
            int has_close = w >= 64;
            int room = w - 34 - (has_close ? BTN_CLOSE_W : 0);

            label[0] = '\0';
            if (room > 0)
                fit_text(label, sizeof(label), t->title, room);
            draw_text(sc.panel, x + 26, bl, fg, label);

            /* close (X) at the right end of a wide enough task button */
            if (has_close) {
                int zx = x + w - 4 - BTN_CLOSE_W;
                int zy = y0 + 4;
                int zh = cfg.panel_height - 8;
                int on = bi == hover_task && hover_task_close;

                if (on) {
                    XSetForeground(sc.dpy, sc.gc, cfg.col_close_hover);
                    XFillRectangle(sc.dpy, sc.panel, sc.gc, zx, zy,
                                   (unsigned)BTN_CLOSE_W, (unsigned)zh);
                }
                XSetForeground(sc.dpy, sc.gc, on ? 0xedf0f5UL : fg);
                {
                    int cx = zx + BTN_CLOSE_W / 2;
                    int cy = zy + zh / 2;

                    XDrawLine(sc.dpy, sc.panel, sc.gc, cx - 4, cy - 4,
                              cx + 4, cy + 4);
                    XDrawLine(sc.dpy, sc.panel, sc.gc, cx - 4, cy + 4,
                              cx + 4, cy - 4);
                }
            }
        }
        x += w;
    }

    /* right side: tray, clock, show desktop (laid out left to right) */
    {
        int rx = sc.sw - desk_w;
        int clock_x0 = rx - clock_w;
        int bl = baseline(y0);
        int cy = y0 + cfg.panel_height / 2;

        if (clock_x0 < x + 8)
            clock_x0 = x + 8;

        if (tray_draw) {
            int tx = clock_x0 - tray_w;
            if (tx < x + 8)
                tx = x + 8;

            if (hover_tray == 0) {
                XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
                XFillRectangle(sc.dpy, sc.panel, sc.gc, tx, y0 + 4,
                               (unsigned)TRAY_ICON_W,
                               (unsigned)(cfg.panel_height - 8));
            }
            icon_network(tx + 5, cy, cfg.col_panel_fg, cfg.col_panel_bg);
            tx += TRAY_ICON_W;
            if (hover_tray == 1) {
                XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
                XFillRectangle(sc.dpy, sc.panel, sc.gc, tx, y0 + 4,
                               (unsigned)TRAY_ICON_W,
                               (unsigned)(cfg.panel_height - 8));
            }
            icon_volume(tx + 5, cy, cfg.col_panel_fg);
            tx += TRAY_ICON_W;
            if (hover_tray == 2) {
                XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
                XFillRectangle(sc.dpy, sc.panel, sc.gc, tx, y0 + 4,
                               (unsigned)TRAY_ICON_W,
                               (unsigned)(cfg.panel_height - 8));
            }
            icon_battery(tx + 5, cy, cfg.col_panel_fg, cfg.col_btn_inactive);
            tx += TRAY_ICON_W;
            if (hover_tray == 3) {
                XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
                XFillRectangle(sc.dpy, sc.panel, sc.gc, tx, y0 + 4,
                               (unsigned)TRAY_ICON_W,
                               (unsigned)(cfg.panel_height - 8));
            }
            icon_shield(tx + 4, cy,
                        scde_security_get_scan_mode() == SCDE_SCAN_OFF
                            ? dim_color(cfg.col_panel_fg, cfg.col_panel_bg)
                            : cfg.col_border_focus);
        }

        if (clock_draw && clock_str[0]) {
            int cx;

            if (two_line_clock() && date_str[0]) {
                int half = cfg.panel_height / 2;
                int base1 = y0 + half - 1 - sc.font_descent / 2;
                int base2 = y0 + cfg.panel_height - 5 - sc.font_descent;

                cx = clock_x0 + (clock_w - text_width(clock_str)) / 2;
                draw_text(sc.panel, cx, base1, cfg.col_panel_fg, clock_str);
                cx = clock_x0 + (clock_w - text_width(date_str)) / 2;
                draw_text(sc.panel, cx, base2, cfg.col_panel_fg, date_str);
            } else {
                cx = rx - 10 - text_width(clock_str);
                if (cx < x)
                    cx = x;
                draw_text(sc.panel, cx, bl, cfg.col_panel_fg, clock_str);
            }
        }

        if (cfg.show_desktop_btn) {
            if (hover_desktop) {
                XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
                XFillRectangle(sc.dpy, sc.panel, sc.gc, rx, 0,
                               (unsigned)DESKTOP_W,
                               (unsigned)cfg.panel_height);
            }
            XSetForeground(sc.dpy, sc.gc, cfg.col_panel_fg);
            XDrawLine(sc.dpy, sc.panel, sc.gc, rx, 4, rx,
                      cfg.panel_height - 4);
        }
    }

    /* Harbor hairline on the desktop-facing edge, quiet separation */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    if (cfg.panel_top)
        XDrawLine(sc.dpy, sc.panel, sc.gc, 0, cfg.panel_height - 1, sc.sw,
                  cfg.panel_height - 1);
    else
        XDrawLine(sc.dpy, sc.panel, sc.gc, 0, 0, sc.sw, 0);
}

void panel_expose(XExposeEvent *ev)
{
    if (ev->count == 0)
        panel_update();
}

int panel_button_press(XButtonEvent *ev)
{
    int i, rx;

    if (ev->button == Button4 || ev->button == Button5)
        return 1;

    if (ev->button != Button1)
        return 1;

    if (ev->x < LAUNCHER_W) {
        if (menu_is_open())
            menu_hide();
        else
            menu_show();
        return 1;
    }

    for (i = 0; i < nbtns; i++) {
        if (ev->x >= btns[i].x && ev->x < btns[i].x + btns[i].w) {
            if (btns[i].w >= 64) {
                int zx = btns[i].x + btns[i].w - 4 - BTN_CLOSE_W;

                if (ev->x >= zx && ev->x < zx + BTN_CLOSE_W) {
                    taskbar_close(btns[i].task);
                    return 1;
                }
            }
            taskbar_activate(btns[i].task);
            return 1;
        }
    }

    if (tray_draw) {
        int tx = sc.sw - desk_w - clock_w - tray_w;

        if (ev->x >= tx && ev->x < tx + tray_w) {
            if ((ev->x - tx) / TRAY_ICON_W == 3)
                security_gui_toggle();          /* shield: Security Center */
            return 1;
        }
    }

    rx = sc.sw - desk_w;
    if (cfg.show_desktop_btn && ev->x >= rx) {
        show_desktop_toggle();
        return 1;
    }
    return 1;
}

void panel_motion(XMotionEvent *ev)
{
    int i, found = -1;
    int lch = (ev->x < LAUNCHER_W);
    int dch = 0;
    int tch = -1;
    int tcc = 0;

    for (i = 0; i < nbtns; i++) {
        if (ev->x >= btns[i].x && ev->x < btns[i].x + btns[i].w) {
            found = i;
            break;
        }
    }
    if (found >= 0 && btns[found].w >= 64) {
        int zx = btns[found].x + btns[found].w - 4 - BTN_CLOSE_W;

        tcc = ev->x >= zx && ev->x < zx + BTN_CLOSE_W;
    }
    if (cfg.show_desktop_btn && ev->x >= sc.sw - desk_w)
        dch = 1;
    if (tray_draw) {
        int tx = sc.sw - desk_w - clock_w - tray_w;
        for (i = 0; i < 4; i++) {
            if (ev->x >= tx + i * TRAY_ICON_W &&
                ev->x < tx + (i + 1) * TRAY_ICON_W)
                tch = i;
        }
    }

    if (lch != hover_launcher || dch != hover_desktop || tch != hover_tray ||
        found != hover_task || tcc != hover_task_close) {
        hover_launcher = lch;
        hover_desktop = dch;
        hover_tray = tch;
        hover_task = found;
        hover_task_close = tcc;
        panel_update();
    }
}

void panel_tick(void)
{
    char now[128];

    if (!sc.panel || !cfg.show_clock)
        return;
    format_now(now, sizeof(now), cfg.clock_format);
    if (strcmp(now, clock_str) != 0)
        panel_update();
}

int panel_timeout_ms(void)
{
    struct timespec ts;
    struct tm tm;
    int ms;

    if (!cfg.show_clock)
        return -1;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 1000;
    localtime_r(&ts.tv_sec, &tm);

    if (clock_has_seconds)
        ms = 1000 - (int)(ts.tv_nsec / 1000000);
    else
        ms = (60 - tm.tm_sec) * 1000 - (int)(ts.tv_nsec / 1000000);
    if (ms <= 0)
        ms = 50;
    return ms;
}
