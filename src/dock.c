#include "scde.h"

#include <ctype.h>
#include <string.h>

#define DOCK_MARGIN  8
#define DOCK_PAD     10
#define DOCK_TILE    50
#define DOCK_GAP     8
#define DOCK_H       72
#define DOCK_CUT     18
#define DOCK_DOT_Y   64
#define DOCK_MAX     (MAX_PANEL_BTNS + 8)
#define PIN_COUNT    5

#define PIN_LAUNCHER (-2)
#define PIN_FILES    (-3)
#define PIN_TERM     (-4)
#define PIN_BROWSER  (-5)
#define PIN_SECURITY (-6)

typedef struct {
    int x;
    int task;
    int pin;
    int kind;
} DockItem;

static DockItem ditems[DOCK_MAX];
static int nditems;
static int dock_x, dock_y, dock_w;
static int hover_dock = -1;
static unsigned long corner_bg;

static void app_initial(const char *title, char *out, size_t out_size)
{
    const char *p;

    if (out_size == 0)
        return;
    out[0] = '\0';
    if (!title)
        return;
    for (p = title; *p; p++) {
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

static void dock_layout(void)
{
    static const int pins[PIN_COUNT] = {
        PIN_LAUNCHER, PIN_FILES, PIN_TERM, PIN_BROWSER, PIN_SECURITY
    };
    int n, shown, max_shown, w, i;

    taskbar_sync();
    n = taskbar_count();
    max_shown = (sc.sw - 120) / (DOCK_TILE + DOCK_GAP);
    if (max_shown < PIN_COUNT + 1)
        max_shown = PIN_COUNT;
    shown = n;
    if (shown > max_shown - PIN_COUNT)
        shown = max_shown - PIN_COUNT;
    if (shown < 0)
        shown = 0;
    if (shown > DOCK_MAX - PIN_COUNT)
        shown = DOCK_MAX - PIN_COUNT;

    nditems = 0;
    for (i = 0; i < PIN_COUNT; i++) {
        ditems[nditems].pin = pins[i];
        ditems[nditems].task = -1;
        ditems[nditems].kind = UI_ICON_GENERIC;
        nditems++;
    }
    for (i = 0; i < shown; i++) {
        ditems[nditems].pin = -1;
        ditems[nditems].task = i;
        ditems[nditems].kind = UI_ICON_GENERIC;
        nditems++;
    }

    w = 2 * DOCK_PAD + nditems * DOCK_TILE +
        (nditems > 1 ? (nditems - 1) * DOCK_GAP : 0);
    {
        int tx = DOCK_PAD;

        for (i = 0; i < nditems; i++) {
            ditems[i].x = tx;
            tx += DOCK_TILE + DOCK_GAP;
        }
    }
    dock_w = w;
    dock_x = (sc.sw - w) / 2;
    dock_y = sc.sh - DOCK_MARGIN - DOCK_H;
    if (dock_x < 4)
        dock_x = 4;
    corner_bg = ui_wallpaper_color(dock_y + DOCK_H - 1);
}

int dock_reserved(void)
{
    return DOCK_H + DOCK_MARGIN;
}

static int pin_active(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t || !t->focused)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return 1;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return 1;
    }
    return 0;
}

static int pin_running(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return 1;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return 1;
    }
    return 0;
}

static int pin_hidden(int pin)
{
    int i, n = taskbar_count();

    for (i = 0; i < n; i++) {
        const TaskEntry *t = taskbar_at(i);

        if (!t)
            continue;
        if (pin == PIN_FILES && t->app_type == TASK_APP_FM)
            return t->hidden;
        if (pin == PIN_SECURITY && t->app_type == TASK_APP_SECURITY)
            return t->hidden;
    }
    return 0;
}

static void dock_paint(void)
{
    int i;
    XSetForeground(sc.dpy, sc.gc, corner_bg);
    XFillRectangle(sc.dpy, sc.dock, sc.gc, 0, 0, (unsigned)dock_w,
                   (unsigned)DOCK_H);

    ui_round_fill(sc.dock, 0, 0, dock_w, DOCK_H, DOCK_CUT,
                  ui_mix(cfg.col_panel_bg, 0xffffffUL, 9));
    ui_round_outline(sc.dock, 0, 0, dock_w, DOCK_H, DOCK_CUT,
                     ui_mix(cfg.col_panel_bg, 0xffffffUL, 20));
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0xffffffUL, 26));
    XDrawLine(sc.dpy, sc.dock, sc.gc, DOCK_CUT, 1, dock_w - DOCK_CUT, 1);
    XSetForeground(sc.dpy, sc.gc, ui_mix(cfg.col_panel_bg, 0x000000UL, 48));
    XDrawLine(sc.dpy, sc.dock, sc.gc, DOCK_CUT, DOCK_H - 2,
              dock_w - DOCK_CUT, DOCK_H - 2);

    for (i = 0; i < nditems; i++) {
        int hovered = (i == hover_dock);
        int size = hovered ? DOCK_TILE + 8 : DOCK_TILE;
        int tx = ditems[i].x - (size - DOCK_TILE) / 2;
        int ty = DOCK_H - 8 - size;
        int running = 0, hidden = 0, focused = 0;
        const TaskEntry *t = NULL;
        const char *title = NULL;
        char initial[2];

        if (ditems[i].pin != -1) {
            int pin = ditems[i].pin;

            if (pin == PIN_LAUNCHER) {
                ui_draw_app_icon(sc.dock, UI_ICON_LAUNCHER, tx, ty, size, 0,
                                 "S");
                continue;
            }
            if (pin == PIN_FILES) {
                running = pin_running(pin);
                hidden = pin_hidden(pin);
                focused = pin_active(pin);
                ui_draw_app_icon(sc.dock, UI_ICON_FILES, tx, ty, size,
                                 hidden, "F");
            } else if (pin == PIN_TERM) {
                ui_draw_app_icon(sc.dock, UI_ICON_TERMINAL, tx, ty, size, 0,
                                 "T");
            } else if (pin == PIN_BROWSER) {
                ui_draw_app_icon(sc.dock, UI_ICON_BROWSER, tx, ty, size, 0,
                                 "B");
            } else {
                running = pin_running(PIN_SECURITY);
                hidden = pin_hidden(PIN_SECURITY);
                focused = pin_active(PIN_SECURITY);
                ui_draw_app_icon(sc.dock, UI_ICON_SECURITY, tx, ty, size,
                                 hidden, "S");
            }
        } else {
            t = taskbar_at(ditems[i].task);
            if (!t)
                continue;
            running = 1;
            hidden = t->hidden;
            focused = t->focused;
            title = t->title;
            app_initial(title, initial, sizeof(initial));
            ui_draw_app_icon(sc.dock, UI_ICON_GENERIC, tx, ty, size,
                             hidden, initial[0] ? initial : "?");
        }

        if (running) {
            unsigned long dot = focused ? ui_accent()
                                        : ui_mix(cfg.col_panel_fg,
                                                 cfg.col_panel_bg, 28);
            int ds = focused ? 5 : 4;

            if (hidden)
                dot = ui_mix(dot, cfg.col_panel_bg, 55);
            XSetForeground(sc.dpy, sc.gc, dot);
            XFillArc(sc.dpy, sc.dock, sc.gc,
                     ditems[i].x + DOCK_TILE / 2 - ds / 2, DOCK_DOT_Y,
                     (unsigned)ds, (unsigned)ds, 0, 360 * 64);
        }
    }
    XFlush(sc.dpy);
}

void dock_create(void)
{
    XSetWindowAttributes wa;

    dock_layout();
    wa.override_redirect = True;
    wa.background_pixel = corner_bg;
    wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask;

    sc.dock = XCreateWindow(sc.dpy, sc.root, dock_x, dock_y,
                            (unsigned)dock_w, (unsigned)DOCK_H, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask,
                            &wa);
    XStoreName(sc.dpy, sc.dock, "scde-dock");
    ui_shape_round(sc.dock, dock_w, DOCK_H, DOCK_CUT);
    XMapRaised(sc.dpy, sc.dock);
    dock_paint();
}

void dock_destroy(void)
{
    if (sc.dock) {
        XDestroyWindow(sc.dpy, sc.dock);
        sc.dock = None;
    }
}

void dock_update(void)
{
    int old_x = dock_x, old_y = dock_y, old_w = dock_w;
    unsigned long old_corner = corner_bg;

    if (!sc.dock)
        return;
    dock_layout();
    if (hover_dock >= nditems)
        hover_dock = -1;
    if (dock_x != old_x || dock_y != old_y || dock_w != old_w) {
        XMoveResizeWindow(sc.dpy, sc.dock, dock_x, dock_y, (unsigned)dock_w,
                          (unsigned)DOCK_H);
        ui_shape_round(sc.dock, dock_w, DOCK_H, DOCK_CUT);
        if (corner_bg != old_corner)
            XSetWindowBackground(sc.dpy, sc.dock, corner_bg);
    }
    dock_paint();
    XFlush(sc.dpy);
}

void dock_raise(void)
{
    if (sc.dock)
        XRaiseWindow(sc.dpy, sc.dock);
}

void dock_resample_bg(void)
{
    dock_update();
}

void dock_expose(XExposeEvent *ev)
{
    if (!sc.dock || ev->window != sc.dock)
        return;
    if (ev->count == 0)
        dock_update();
}

static int dock_hit(int x)
{
    int i;

    for (i = 0; i < nditems; i++) {
        if (x >= ditems[i].x && x < ditems[i].x + DOCK_TILE)
            return i;
    }
    return -1;
}

static void pin_activate(int pin)
{
    switch (pin) {
    case PIN_LAUNCHER:
        if (menu_is_open())
            menu_hide();
        else
            menu_show();
        break;
    case PIN_FILES:
        fm_show();
        break;
    case PIN_TERM:
        apps_run_terminal("terminal");
        break;
    case PIN_BROWSER:
        apps_run("browser", apps_browser());
        break;
    case PIN_SECURITY:
        security_gui_show();
        break;
    default:
        break;
    }
}

static void pin_minimize(int pin)
{
    if (pin == PIN_FILES) {
        if (fm_open())
            fm_hide();
        return;
    }
    if (pin == PIN_SECURITY) {
        if (security_gui_open())
            security_gui_hide();
        return;
    }
}

int dock_button_press(XButtonEvent *ev)
{
    int i;

    if (!sc.dock || ev->window != sc.dock)
        return 0;
    if (ev->button == Button4 || ev->button == Button5)
        return 1;

    i = dock_hit(ev->x);
    if (i < 0)
        return 1;

    if (ditems[i].pin != -1) {
        int pin = ditems[i].pin;

        if (ev->button == Button1)
            pin_activate(pin);
        else if (ev->button == Button3)
            pin_minimize(pin);
        else if (ev->button == Button2 &&
                 (pin == PIN_FILES || pin == PIN_SECURITY)) {
            if (pin == PIN_FILES)
                fm_close();
            else
                security_gui_close();
        }
        return 1;
    }

    if (ev->button == Button1)
        taskbar_activate(ditems[i].task);
    else if (ev->button == Button2)
        taskbar_close(ditems[i].task);
    else if (ev->button == Button3) {
        const TaskEntry *t = taskbar_at(ditems[i].task);

        if (t) {
            if (t->client)
                client_minimize(t->client, t->hidden ? 0 : 1);
            else if (t->app_type == TASK_APP_FM) {
                if (fm_open())
                    fm_hide();
                else
                    fm_show();
            } else if (t->app_type == TASK_APP_SECURITY) {
                if (security_gui_open())
                    security_gui_hide();
                else
                    security_gui_show();
            }
        }
    }
    return 1;
}

void dock_motion(XMotionEvent *ev)
{
    int found;

    if (!sc.dock || ev->window != sc.dock)
        return;
    found = dock_hit(ev->x);
    if (found != hover_dock) {
        hover_dock = found;
        dock_update();
    }
}
