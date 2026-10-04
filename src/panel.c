/*
 * SCDE - panel (Plasma style): launcher, task manager, tray, clock
 */
#include "scde.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define LAUNCHER_W 44
#define TRAY_ICON_W 24
#define DESKTOP_W 8
#define BTN_MAX_W 176
#define BTN_MIN_W 44

typedef struct {
    Client *c;
    int x, w;
} PanelBtn;

static PanelBtn btns[MAX_PANEL_BTNS];
static int nbtns;
static char clock_str[128];
static char date_str[128];
static int hover_launcher;
static int hover_desktop;
static int hover_task = -1;
static int hover_tray = -1;
static int clock_has_seconds;
static int clock_w, tray_w, desk_w;

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
    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_fg);
    for (i = 0; i < 2; i++)
        for (j = 0; j < 2; j++)
            XFillRectangle(sc.dpy, sc.panel, sc.gc, x0 + i * (s + g),
                           y0i + j * (s + g), (unsigned)s, (unsigned)s);
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
    Client *c;

    if (!sc.panel)
        return;

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
    tray_w = cfg.show_tray ? TRAY_ICON_W * 3 : 0;
    desk_w = cfg.show_desktop_btn ? DESKTOP_W : 0;

    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, sc.panel, sc.gc, 0, 0, (unsigned)sc.panel_w,
                   (unsigned)cfg.panel_height);

    /* launcher */
    if (hover_launcher) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
        XFillRectangle(sc.dpy, sc.panel, sc.gc, 2, 2,
                       (unsigned)(LAUNCHER_W - 4),
                       (unsigned)(cfg.panel_height - 4));
    }
    icon_launcher(y0);
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XDrawRectangle(sc.dpy, sc.panel, sc.gc, LAUNCHER_W, 4, 0,
                   (unsigned)(cfg.panel_height - 8));

    x = LAUNCHER_W + 6;
    right = sc.sw - clock_w - tray_w - desk_w - 8;
    avail = right - x;
    if (avail < 0)
        avail = 0;

    n = 0;
    for (c = sc.clients; c && n < MAX_PANEL_BTNS; c = c->next)
        n++;

    w = (n > 0 && avail > 0) ? avail / n : BTN_MAX_W;
    if (w > BTN_MAX_W)
        w = BTN_MAX_W;
    shown = n;
    if (n > 0 && w < BTN_MIN_W) {
        shown = avail / BTN_MIN_W;
        if (shown < 1)
            shown = 1;
        if (shown > n)
            shown = n;
        w = avail / shown;
        if (w > BTN_MAX_W)
            w = BTN_MAX_W;
    }

    nbtns = 0;
    for (c = sc.clients, i = 0; c && i < shown; c = c->next, i++) {
        char label[NAME_MAX_LEN];
        int active = (c == sc.focused);
        unsigned long bg;
        int bl;

        if (x + 2 >= right)
            break;
        btns[nbtns].c = c;
        btns[nbtns].x = x;
        btns[nbtns].w = w;
        nbtns++;

        bg = active ? cfg.col_btn_active : cfg.col_btn_inactive;
        XSetForeground(sc.dpy, sc.gc, bg);
        XFillRectangle(sc.dpy, sc.panel, sc.gc, x, y0 + 4, (unsigned)(w - 4),
                       (unsigned)(cfg.panel_height - 8));
        if (active) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
            XFillRectangle(sc.dpy, sc.panel, sc.gc, x, y0 + 4, (unsigned)(w - 4),
                           2);
        }

        bl = baseline(y0);
        fit_text(label, sizeof(label), c->name, w - 18);
        draw_text(sc.panel, x + 8, bl,
                  active ? 0xffffffUL : cfg.col_panel_fg, label);
        if (c->minimized) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_panel_fg);
            XDrawRectangle(sc.dpy, sc.panel, sc.gc, x + 2, y0 + 6, 4, 4);
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

        if (cfg.show_tray) {
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
        }

        if (cfg.show_clock && clock_str[0]) {
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

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
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
            if (btns[i].c->minimized)
                client_minimize(btns[i].c, 0);
            focus_client(btns[i].c);
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

    for (i = 0; i < nbtns; i++) {
        if (ev->x >= btns[i].x && ev->x < btns[i].x + btns[i].w) {
            found = i;
            break;
        }
    }
    if (cfg.show_desktop_btn && ev->x >= sc.sw - desk_w)
        dch = 1;
    if (cfg.show_tray) {
        int tx = sc.sw - desk_w - clock_w - tray_w;
        for (i = 0; i < 3; i++) {
            if (ev->x >= tx + i * TRAY_ICON_W &&
                ev->x < tx + (i + 1) * TRAY_ICON_W)
                tch = i;
        }
    }

    if (lch != hover_launcher || dch != hover_desktop || tch != hover_tray ||
        found != hover_task) {
        hover_launcher = lch;
        hover_desktop = dch;
        hover_tray = tch;
        hover_task = found;
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
