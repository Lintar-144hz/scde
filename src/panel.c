#include "scde.h"

#include <scde/security.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define BAR_MARK_W   34
#define BAR_ITEM_PAD 10
#define TRAY_ICON_W  25
#define BAR_MIN_H    20
#define BAR_MAX_H    40

#define MB_LOGO    0
#define MB_APP     1
#define MB_FILE    2
#define MB_WINDOW  3
#define MB_HELP    4
#define MB_COUNT   5

typedef struct {
    int x, w;
} BarItem;

static BarItem items[MB_COUNT];
static int hover_item = -1;
static int hover_tray = -1;
static int open_kind = -1;
static char clock_str[128];
static char cur_app[NAME_MAX_LEN];
static int clock_has_seconds;
static int clock_w, tray_w;
static int tray_draw, clock_draw;
static int bar_h;

int menubar_h(void)
{
    int h = cfg.panel_height;

    if (h < BAR_MIN_H)
        h = BAR_MIN_H;
    if (h > BAR_MAX_H)
        h = BAR_MAX_H;
    return h;
}

int reserve_top(void)
{
    return menubar_h();
}

int reserve_bottom(void)
{
    return dock_reserved();
}

int panel_reserved(void)
{
    return reserve_top();
}

static void format_now(char *buf, size_t size, const char *fmt)
{
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    if (!fmt || !*fmt || strftime(buf, size, fmt, &tm) == 0)
        buf[0] = '\0';
}

static void bar_paint_bg(void)
{
    int y;

    for (y = 0; y < bar_h; y++) {
        unsigned long c = ui_mix(ui_wallpaper_color(y), cfg.col_panel_bg, 76);

        XSetForeground(sc.dpy, sc.gc, c);
        XFillRectangle(sc.dpy, sc.panel, sc.gc, 0, y, (unsigned)sc.panel_w, 1);
    }
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0xffffffUL, 7));
    XFillRectangle(sc.dpy, sc.panel, sc.gc, 0, 0, (unsigned)sc.panel_w, 1);
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0x000000UL, 45));
    XFillRectangle(sc.dpy, sc.panel, sc.gc, 0, bar_h - 1,
                   (unsigned)sc.panel_w, 1);
}

static void bar_menu_pill(int x, int wd, int open)
{
    unsigned long bg = open ? ui_mix(cfg.col_panel_bg, 0xffffffUL, 17)
                            : ui_mix(cfg.col_panel_bg, 0xffffffUL, 10);

    ui_round_fill(sc.panel, x + 2, 3, wd - 4, bar_h - 6, 5, bg);
}

static void bar_open_menu(int kind, int x)
{
    static MenuEntry win_items[32];
    static MenuEntry file_items[8];
    static MenuEntry help_items[6];
    MenuEntry *list = NULL;
    int n = 0;
    int i, count;

    if (!menu_is_open())
        open_kind = -1;
    if (open_kind == kind) {
        menu_hide();
        open_kind = -1;
        return;
    }
    menu_hide();
    open_kind = kind;

#define ADD(arr, L, C) do { \
        str_copy(arr[n].label, sizeof(arr[0].label), (L)); \
        str_copy(arr[n].cmd, sizeof(arr[0].cmd), (C)); \
        n++; \
    } while (0)

    if (kind == MB_FILE) {
        const char *t = cur_app[0] ? cur_app : "SCDE";
        char label[96];

        snprintf(label, sizeof(label), "About %.48s", t);
        ADD(file_items, label, "@wm-about");
        ADD(file_items, "-", "");
        snprintf(label, sizeof(label), "Hide %.48s", t);
        ADD(file_items, label, "@wm-hide");
        snprintf(label, sizeof(label), "Close %.48s", t);
        ADD(file_items, label, "@wm-close");
        ADD(file_items, "-", "");
        ADD(file_items, "Quit SCDE", "@logout");
        list = file_items;
    } else if (kind == MB_WINDOW) {
        taskbar_sync();
        count = taskbar_count();
        ADD(win_items, "Minimize", "@wm-hide");
        ADD(win_items, "Show Desktop", "@wm-desktop");
        ADD(win_items, "-", "");
        for (i = 0; i < count && n < 30; i++) {
            const TaskEntry *t = taskbar_at(i);
            char cmd[32];

            if (!t)
                break;
            snprintf(cmd, sizeof(cmd), "@wm-focus %d", i);
            ADD(win_items, t->title[0] ? t->title : "Window", cmd);
        }
        list = win_items;
    } else {
        ADD(help_items, "About SCDE", "@wm-about-scde");
        ADD(help_items, "Keyboard Shortcuts", "@wm-keys");
        ADD(help_items, "-", "");
        ADD(help_items, "Security Center", "@security");
        list = help_items;
    }
#undef ADD

    if (list && n > 0)
        menu_show_custom(list, n, x, menubar_h());
}

static const char *display_app_name(void)
{
    if (strcmp(cur_app, "File Manager") == 0)
        return "Files";
    if (strcmp(cur_app, "SCDE Security") == 0)
        return "Security";
    return cur_app;
}

void panel_create(void)
{
    XSetWindowAttributes wa;

    bar_h = menubar_h();
    sc.panel_w = sc.sw;
    wa.override_redirect = True;
    wa.background_pixel = cfg.col_panel_bg;
    wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                    ButtonReleaseMask;

    sc.panel = XCreateWindow(sc.dpy, sc.root, 0, 0, (unsigned)sc.panel_w,
                             (unsigned)bar_h, 0, CopyFromParent, InputOutput,
                             CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &wa);
    XStoreName(sc.dpy, sc.panel, "scde-menubar");
    clock_has_seconds = strchr(cfg.clock_format, 'S') != NULL;
    format_now(clock_str, sizeof(clock_str), cfg.clock_format);

    dock_create();

    XMapRaised(sc.dpy, sc.panel);
    panel_update();
}

void panel_destroy(void)
{
    dock_destroy();
    if (sc.panel) {
        XDestroyWindow(sc.dpy, sc.panel);
        sc.panel = None;
    }
}

void panel_raise(void)
{
    dock_raise();
    if (sc.panel)
        XRaiseWindow(sc.dpy, sc.panel);
}

void panel_update(void)
{
    int x, i, bl;
    const char *active_title;

    if (!sc.panel)
        return;

    taskbar_sync();
    active_title = taskbar_active_title();
    str_copy(cur_app, sizeof(cur_app),
             active_title && *active_title ? active_title : "SCDE");

    bar_h = menubar_h();
    clock_str[0] = '\0';
    if (cfg.show_clock)
        format_now(clock_str, sizeof(clock_str), cfg.clock_format);

    clock_w = cfg.show_clock && clock_str[0] ? text_width(clock_str) + 18 : 0;
    tray_w = cfg.show_tray ? TRAY_ICON_W * 4 : 0;

    tray_draw = tray_w > 0 &&
                sc.sw - clock_w - tray_w - 16 - 260 >= 8;
    clock_draw = clock_w > 0 &&
                 sc.sw - (tray_draw ? tray_w : 0) - clock_w - 16 - 260 >= 8;
    if (!clock_draw)
        tray_draw = 0;

    bar_paint_bg();

    {
        const char *labels[MB_COUNT];
        const char *app = display_app_name();
        int w;
        int right_limit = sc.sw - 16 - (tray_draw ? tray_w : 0) -
                          (clock_draw ? clock_w + 10 : 0);

        labels[MB_LOGO] = NULL;
        labels[MB_APP] = app;
        labels[MB_FILE] = "File";
        labels[MB_WINDOW] = "Window";
        labels[MB_HELP] = "Help";

        x = 8;
        for (i = 0; i < MB_COUNT; i++) {
            if (i == MB_LOGO)
                w = BAR_MARK_W;
            else if (i == MB_APP)
                w = text_width_bold(labels[i]) + 2 * BAR_ITEM_PAD;
            else
                w = text_width(labels[i]) + 2 * BAR_ITEM_PAD;
            if (i == MB_APP && x + w > right_limit) {
                w = right_limit - x;
                if (w < 56)
                    w = 56;
            }
            items[i].x = x;
            items[i].w = w;
            if (i == hover_item || open_kind == i)
                bar_menu_pill(x, w, open_kind == i);
            if (i == MB_LOGO) {
                ui_draw_launcher_mark(sc.panel, x + w / 2, bar_h / 2, 17,
                                      cfg.col_panel_fg);
            } else {
                bl = (bar_h + sc.font_ascent - sc.font_descent) / 2;
                if (i == MB_APP)
                    draw_text_clip_font(sc.panel, sc.font_bold, x + BAR_ITEM_PAD,
                                        x + w - 6, bl, 0xffffffUL, labels[i]);
                else
                    draw_text_clip(sc.panel, x + BAR_ITEM_PAD, x + w - 6, bl,
                                   cfg.col_panel_fg, labels[i]);
            }
            x += w + 1;
        }
    }

    {
        int rx = sc.sw - 10;
        int cy = bar_h / 2;

        bl = (bar_h + sc.font_ascent - sc.font_descent) / 2;

        if (clock_draw && clock_str[0]) {
            int cx = rx - clock_w + (clock_w - text_width(clock_str)) / 2;

            draw_text(sc.panel, cx, bl, cfg.col_panel_fg, clock_str);
        }

        if (tray_draw) {
            int tx = (clock_draw ? rx - clock_w - 8 : rx) - tray_w;

            if (tx < items[MB_HELP].x + items[MB_HELP].w + 8)
                tx = items[MB_HELP].x + items[MB_HELP].w + 8;
            for (i = 0; i < 4; i++) {
                if (hover_tray == i)
                    ui_round_fill(sc.panel, tx + 1, 3, TRAY_ICON_W - 2,
                                  bar_h - 6, 5,
                                  ui_mix(cfg.col_panel_bg, 0xffffffUL, 12));
                if (i == 0)
                    ui_draw_wifi(sc.panel, tx + 5, cy, cfg.col_panel_fg);
                else if (i == 1)
                    ui_draw_volume(sc.panel, tx + 5, cy, cfg.col_panel_fg);
                else if (i == 2)
                    ui_draw_battery(sc.panel, tx + 5, cy, cfg.col_panel_fg,
                                    cfg.col_panel_bg);
                else
                    ui_draw_shield(sc.panel, tx + 5, cy, 14,
                                   scde_security_get_scan_mode() ==
                                           SCDE_SCAN_OFF
                                       ? ui_dim(cfg.col_panel_fg,
                                                cfg.col_panel_bg)
                                       : ui_accent());
                tx += TRAY_ICON_W;
            }
        }
    }

    dock_update();
}

void panel_expose(XExposeEvent *ev)
{
    if (ev->count == 0)
        panel_update();
}

int panel_button_press(XButtonEvent *ev)
{
    int i;

    if (ev->button == Button4 || ev->button == Button5)
        return 1;
    if (ev->button != Button1)
        return 1;
    if (!menu_is_open())
        open_kind = -1;

    for (i = 0; i < MB_COUNT; i++) {
        if (ev->x >= items[i].x && ev->x < items[i].x + items[i].w) {
            if (i == MB_LOGO) {
                if (menu_is_open()) {
                    menu_hide();
                    open_kind = -1;
                } else {
                    menu_show();
                    open_kind = MB_LOGO;
                }
            } else if (i == MB_APP) {
                bar_open_menu(MB_FILE, items[i].x);
            } else {
                bar_open_menu(i, items[i].x);
            }
            panel_update();
            return 1;
        }
    }

    if (tray_draw) {
        int tx = (clock_draw ? sc.sw - 10 - clock_w - 8 : sc.sw - 10) -
                 tray_w;

        if (tx < items[MB_HELP].x + items[MB_HELP].w + 8)
            tx = items[MB_HELP].x + items[MB_HELP].w + 8;
        if (ev->x >= tx && ev->x < tx + tray_w) {
            if ((ev->x - tx) / TRAY_ICON_W == 3)
                security_gui_toggle();
            return 1;
        }
    }
    return 1;
}

void panel_motion(XMotionEvent *ev)
{
    int i, found = -1, tch = -1;

    for (i = 0; i < MB_COUNT; i++) {
        if (ev->x >= items[i].x && ev->x < items[i].x + items[i].w) {
            found = i;
            break;
        }
    }
    if (tray_draw) {
        int tx = (clock_draw ? sc.sw - 10 - clock_w - 8 : sc.sw - 10) -
                 tray_w;

        if (tx < items[MB_HELP].x + items[MB_HELP].w + 8)
            tx = items[MB_HELP].x + items[MB_HELP].w + 8;
        for (i = 0; i < 4; i++) {
            if (ev->x >= tx + i * TRAY_ICON_W &&
                ev->x < tx + (i + 1) * TRAY_ICON_W)
                tch = i;
        }
    }

    if (found != hover_item || tch != hover_tray) {
        hover_item = found;
        hover_tray = tch;
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
