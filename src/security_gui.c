/*
 * SCDE Security Center - the built-in security application window.
 *
 * Override-redirect like fm.c/msgbox.c (self-drawn title bar, no client
 * decoration).  Sections: protection mode + stats, file scanner with
 * bounded targets, permission toggles, scrollable security log.  Opens
 * from the launcher ("Security Center") or the tray shield; never by
 * itself at startup so task button positions stay stable.
 */
#include "scde.h"

#include <scde/event.h>
#include <scde/plugin.h>
#include <scde/security.h>
#include <scde/security_bait.h>
#include <scde/security_mirage.h>
#include <scde/security_resource.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SG_W 580
#define SG_H 600
#define SG_TITLE 24
#define SG_PAD 10
#define SG_LOG_Y 434
#define SG_LOG_ROW 16
#define SG_LOG_ROWS 9
#define SG_SCAN_CAP 32
#define SG_MAX_BTNS 20

typedef struct {
    int x, y, w, h;
    int id;
    char label[32];
} SecBtn;

static Window sg_win;
static int sg_open;
static int sg_x, sg_y;                    /* dragged position */
static SecBtn sg_btns[SG_MAX_BTNS];
static int sg_nbtns;
static int sg_hover = -1;
static int sg_hover_close;
static int sg_log_off;
static char sg_scan_status[196] = "choose a target to scan";

/* internal monotonic clock shared with the audit log */
uint32_t scde_event_now_ms(void);

static const char *pol_names[SCDE_POL_COUNT] = {
    "Allow untrusted plugins",
    "Block known-bad hashes",
    "Deny suspicious files",
    "Require trusted plugins",
    "Audit every file scan"
};

static void sg_draw(void);

/* --------------------------------------------------------------- layout -- */

static void sg_btn(int x, int y, int w, int h, int id, const char *label)
{
    if (sg_nbtns >= SG_MAX_BTNS)
        return;
    sg_btns[sg_nbtns].x = x;
    sg_btns[sg_nbtns].y = y;
    sg_btns[sg_nbtns].w = w;
    sg_btns[sg_nbtns].h = h;
    sg_btns[sg_nbtns].id = id;
    str_copy(sg_btns[sg_nbtns].label, sizeof(sg_btns[0].label), label);
    sg_nbtns++;
}

static void sg_paint_btn(const SecBtn *b, int active, int hover)
{
    int cx = b->x + b->w / 2;
    unsigned long bg = active ? cfg.col_btn_active
                      : hover  ? cfg.col_btn_hover
                               : cfg.col_btn_inactive;

    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, b->x, b->y, (unsigned)b->w,
                   (unsigned)b->h);
    XSetForeground(sc.dpy, sc.gc,
                   active ? cfg.col_border_focus : cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sg_win, sc.gc, b->x, b->y, (unsigned)b->w,
                   (unsigned)b->h);
    draw_text(sg_win, cx - text_width(b->label) / 2,
              b->y + (b->h + sc.font_ascent - sc.font_descent) / 2,
              active ? 0xffffffUL : cfg.col_menu_fg, b->label);
}

static void sg_section(int top, const char *title)
{
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    draw_text(sg_win, SG_PAD, top + sc.font_ascent, cfg.col_border_focus,
              title);
}

/* --------------------------------------------------------------- actions -- */

typedef struct {
    int count, threats, worst;
    char worst_name[64];
} ScanAcc;

static ScanAcc sg_acc;

static void sg_scan_dir(const char *dir, const char *suffix, int cap)
{
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d)) != NULL && sg_acc.count < cap) {
        char path[SCDE_PATH_MAX];
        struct stat st;
        scde_scan_result_t r;
        size_t len;

        if (e->d_name[0] == '.')
            continue;
        if (suffix) {
            len = strlen(e->d_name);
            if (len < strlen(suffix) ||
                strcmp(e->d_name + len - strlen(suffix), suffix) != 0)
                continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (scde_scan_file(path, &r) != SCDE_OK)
            continue;
        sg_acc.count++;
        if ((int)r.status >= SCDE_THREAT_SUSPICIOUS)
            sg_acc.threats++;
        if (r.risk > sg_acc.worst) {
            sg_acc.worst = r.risk;
            str_copy(sg_acc.worst_name, sizeof(sg_acc.worst_name),
                     e->d_name);
        }
    }
    closedir(d);
}

static void sg_scan_finish(void)
{
    if (sg_acc.count == 0)
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "no files found");
    else
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "%d files, %d threats, worst risk %d (%s)",
                 sg_acc.count, sg_acc.threats, sg_acc.worst,
                 sg_acc.worst_name[0] ? sg_acc.worst_name : "-");
}

static void sg_run_scan(int id)
{
    const char *home = getenv("HOME");
    char dir[512];

    memset(&sg_acc, 0, sizeof(sg_acc));
    if (scde_security_get_scan_mode() == SCDE_SCAN_OFF) {
        snprintf(sg_scan_status, sizeof(sg_scan_status),
                 "scanner is off - switch protection on first");
        sg_draw();
        return;
    }
    if (id == 20) {                        /* plugin directories */
        const char *p = scde_plugins_search_path();

        while (*p && sg_acc.count < SG_SCAN_CAP) {
            const char *sep = strchr(p, ':');
            size_t dlen = sep ? (size_t)(sep - p) : strlen(p);

            if (dlen > 0 && dlen < sizeof(dir)) {
                memcpy(dir, p, dlen);
                dir[dlen] = '\0';
                sg_scan_dir(dir, ".so", SG_SCAN_CAP);
            }
            if (!sep)
                break;
            p = sep + 1;
        }
    } else if (home && *home) {
        if (id == 21)                      /* ~/Desktop */
            snprintf(dir, sizeof(dir), "%s/Desktop", home);
        else                               /* ~/ top-level files */
            snprintf(dir, sizeof(dir), "%s", home);
        sg_scan_dir(dir, NULL, SG_SCAN_CAP);
    }
    sg_scan_finish();
    sg_draw();
}

static void sg_set_mode(int id)
{
    scde_scan_mode_t m = id == 10 ? SCDE_SCAN_ALWAYS_ON
                       : id == 11 ? SCDE_SCAN_PASSIVE
                                  : SCDE_SCAN_OFF;

    scde_security_set_scan_mode(m);
    if (m == SCDE_SCAN_ALWAYS_ON)
        scde_security_watch_defaults();
    panel_update();                        /* tray shield colour */
    sg_draw();
}

/* Detection toggles: user keeps full control.  Enabling lazily creates
 * only tmp-scoped dummy decoys (never user data); disabling leaves the
 * decoys inert but on disk until shutdown. */
static void sg_toggle_bait(void)
{
    if (!scde_bait_active()) {
        if (scde_bait_init(NULL) == SCDE_OK)
            scde_bait_create_defaults();
    } else if (scde_bait_enabled()) {
        scde_bait_disable();
    } else {
        scde_bait_enable();
    }
    sg_draw();
}

static void sg_toggle_mirage(void)
{
    if (!scde_mirage_active()) {
        if (scde_mirage_init() == SCDE_OK)
            scde_mirage_create_defaults();
    } else if (scde_mirage_enabled()) {
        scde_mirage_disable();
    } else {
        scde_mirage_enable();
    }
    sg_draw();
}

/* Short prefix so decoy/sandbox/policy hits read like the spec examples:
 * [BAIT] suspicious process accessed decoy, [MIRAGE] probe, ... */
static const char *sg_prefix(int event)
{
    switch (event) {
    case SCDE_AUDIT_BAIT_CREATED:
    case SCDE_AUDIT_BAIT_ACCESSED:
    case SCDE_AUDIT_BAIT_MODIFIED:
    case SCDE_AUDIT_BAIT_DELETED:
    case SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT:
    case SCDE_AUDIT_BAIT_PERMISSION_DENIED:
        return "[BAIT]";
    case SCDE_AUDIT_MIRAGE_CREATED:
    case SCDE_AUDIT_MIRAGE_PROBED:
    case SCDE_AUDIT_MIRAGE_ACCESSED:
    case SCDE_AUDIT_MIRAGE_MODIFIED:
    case SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT:
        return "[MIRAGE]";
    case SCDE_AUDIT_SANDBOX_CREATED:
    case SCDE_AUDIT_SANDBOX_FAILED:
    case SCDE_AUDIT_PROCESS_RESTRICTED:
    case SCDE_AUDIT_PROCESS_BLOCKED:
        return "[SANDBOX]";
    case SCDE_AUDIT_CAPABILITY_GRANTED:
    case SCDE_AUDIT_CAPABILITY_DENIED:
    case SCDE_AUDIT_POLICY_DENIED:
    case SCDE_AUDIT_TRUST_REQUIRED:
    case SCDE_AUDIT_POLICY_FALLBACK:
    case SCDE_AUDIT_SECURITY_FEATURE_UNAVAILABLE:
        return "[POLICY]";
    default:
        return "[SCAN]";
    }
}

/* ---------------------------------------------------------------- paint -- */

static void sg_draw(void)
{
    scde_scan_stats_t st;
    scde_scan_mode_t mode;
    int rules = 0, hashes = 0, trusted = 0;
    int watch_on;
    int i;
    char buf[196];
    const char *mode_names[3] = { "always-on", "passive", "off" };

    if (!sg_win || !sg_open)
        return;
    sg_nbtns = 0;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, 0, 0, SG_W, SG_H);

    /* title bar */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, 0, 0, SG_W, SG_TITLE);
    draw_text(sg_win, 8, 16, cfg.col_title_fg, "SCDE Security");
    if (sg_hover_close) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_close_hover);
        XFillRectangle(sc.dpy, sg_win, sc.gc, SG_W - SG_TITLE, 0,
                       SG_TITLE, SG_TITLE);
    }
    XSetForeground(sc.dpy, sc.gc, 0xffffffUL);
    XDrawLine(sc.dpy, sg_win, sc.gc, SG_W - 16, 8, SG_W - 8, 16);
    XDrawLine(sc.dpy, sg_win, sc.gc, SG_W - 16, 16, SG_W - 8, 8);

    /* protection */
    sg_section(34, "Protection");
    sg_btn(SG_PAD, 54, 92, 22, 10, "Always-on");
    sg_btn(SG_PAD + 100, 54, 92, 22, 11, "Passive");
    sg_btn(SG_PAD + 200, 54, 92, 22, 12, "Off");
    mode = scde_security_get_scan_mode();
    watch_on = scde_security_watch_fd() >= 0;
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 10 && sg_btns[i].id <= 12)
            sg_paint_btn(&sg_btns[i], (sg_btns[i].id - 10) == (int)mode,
                         sg_hover == i);

    snprintf(buf, sizeof(buf), "Protection: %s    watcher: %s",
             mode_names[(int)mode], watch_on ? "active" : "off");
    draw_text(sg_win, SG_PAD, 94, cfg.col_menu_fg, buf);
    scde_security_stats(&st);
    snprintf(buf, sizeof(buf),
             "scans %u    threats %u    blocked %u    denied %u    "
             "rejected plugins %u",
             (unsigned)st.scans, (unsigned)st.threats,
             (unsigned)st.blocked, (unsigned)st.denied,
             (unsigned)st.rejected_plugins);
    draw_text(sg_win, SG_PAD, 110, cfg.col_menu_fg, buf);
    scde_security_db_info(&rules, &hashes, &trusted);
    snprintf(buf, sizeof(buf), "rules %d    bad hashes %d    trusted %d",
             rules, hashes, trusted);
    draw_text(sg_win, SG_PAD, 126, cfg.col_menu_fg, buf);

    /* honest enforcement status: what the kernel/sandbox can really do
     * here (probes ran once at init) - never claim isolation we lack */
    {
        scde_security_features_t f;

        scde_security_features(&f);
        snprintf(buf, sizeof(buf),
                 "kernel: %s   sandbox: %s   scanner: %s%s",
                 scde_feature_state_name(f.kernel_enforcement),
                 scde_feature_state_name(f.sandbox),
                 mode == SCDE_SCAN_OFF ? "off" : "active",
                 f.proot ? "   (proot: limited)" : "");
        draw_text(sg_win, SG_PAD, 136, cfg.col_border_unfocus, buf);
    }
    {
        char lim[96];

        scde_resource_describe(lim, sizeof(lim));
        fit_text(buf, sizeof(buf), lim, SG_W - 2 * SG_PAD);
        draw_text(sg_win, SG_PAD, 150, cfg.col_border_unfocus, buf);
    }

    /* detection (advisory only: bait/mirage detect, they never isolate) */
    sg_section(160, "Detection (advisory, not a sandbox)");
    {
        int bait_on = scde_bait_enabled();
        int mir_on = scde_mirage_enabled();

        snprintf(buf, sizeof(buf), "Bait: %s  hits: %d",
                 !scde_bait_active() ? "OFF" : bait_on ? "ON" : "OFF",
                 scde_bait_hits());
        draw_text(sg_win, SG_PAD, 182, cfg.col_menu_fg, buf);
        sg_btn(SG_W - SG_PAD - 118, 168, 56, 16, 30,
               bait_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], bait_on,
                     sg_hover == sg_nbtns - 1);
        sg_btn(SG_W - SG_PAD - 56, 168, 56, 16, 32, "Check");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], 0,
                     sg_hover == sg_nbtns - 1);

        snprintf(buf, sizeof(buf), "Mirage: %s  probes: %d",
                 !scde_mirage_active() ? "OFF" : mir_on ? "ON" : "OFF",
                 scde_mirage_probes());
        draw_text(sg_win, SG_PAD, 202, cfg.col_menu_fg, buf);
        sg_btn(SG_W - SG_PAD - 118, 188, 56, 16, 31,
               mir_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], mir_on,
                     sg_hover == sg_nbtns - 1);
    }

    /* scanner */
    sg_section(212, "Scanner");
    sg_btn(SG_PAD, 230, 104, 22, 20, "Scan plugins");
    sg_btn(SG_PAD + 112, 230, 104, 22, 21, "Scan Desktop");
    sg_btn(SG_PAD + 224, 230, 104, 22, 22, "Scan Home");
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 20 && sg_btns[i].id <= 22)
            sg_paint_btn(&sg_btns[i], 0, sg_hover == i);
    fit_text(buf, sizeof(buf), sg_scan_status, SG_W - 2 * SG_PAD);
    draw_text(sg_win, SG_PAD, 270, cfg.col_menu_fg, buf);

    /* permissions */
    sg_section(284, "Permissions");
    for (i = 0; i < SCDE_POL_COUNT; i++) {
        int y = 304 + i * 20;
        int on = scde_policy_get(i);

        draw_text(sg_win, SG_PAD, y + 14, cfg.col_menu_fg, pol_names[i]);
        sg_btn(SG_W - SG_PAD - 56, y + 2, 56, 16, 100 + i,
               on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], on, sg_hover == sg_nbtns - 1);
    }

    /* log */
    sg_section(414, "Log");
    sg_btn(SG_W - SG_PAD - 60, 410, 60, 18, 200, "Clear");
    sg_paint_btn(&sg_btns[sg_nbtns - 1], 0, sg_hover == sg_nbtns - 1);
    {
        int n = scde_audit_count();
        int end = n - sg_log_off;
        int start;
        uint32_t now = scde_event_now_ms();

        if (end > n)
            end = n;
        if (end < 0)
            end = 0;
        start = end - SG_LOG_ROWS;
        if (start < 0)
            start = 0;
        if (n == 0)
            draw_text(sg_win, SG_PAD, SG_LOG_Y + 12,
                      cfg.col_border_unfocus, "audit log is empty");
        for (i = start; i < end; i++) {
            scde_audit_entry_t e;
            int row = SG_LOG_Y + (i - start) * SG_LOG_ROW;
            unsigned long age;
            unsigned long color;
            char line[196];

            if (scde_audit_get(i, &e) != SCDE_OK)
                continue;
            age = (unsigned long)((now - e.time_ms) / 1000u);
            color = e.risk >= 50 ? cfg.col_close_hover : cfg.col_menu_fg;
            snprintf(line, sizeof(line), "%s %lu s  %s  r%d  %s",
                     sg_prefix(e.event), age,
                     scde_audit_event_name(e.event), e.risk, e.subject);
            fit_text(buf, sizeof(buf), line, SG_W - 2 * SG_PAD);
            draw_text(sg_win, SG_PAD, row + 12, color, buf);
        }
    }

    /* focus border */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XDrawRectangle(sc.dpy, sg_win, sc.gc, 0, 0, (unsigned)(SG_W - 1),
                   (unsigned)(SG_H - 1));
    XFlush(sc.dpy);
}

/* --------------------------------------------------------------- public -- */

void security_gui_show(void)
{
    if (!sg_win) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_menu_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        KeyPressMask;
        sg_x = (sc.sw - SG_W) / 2;
        sg_y = (sc.sh - SG_H) / 2 - 40 > 10 ? (sc.sh - SG_H) / 2 - 40 : 10;
        sg_win = XCreateWindow(sc.dpy, sc.root, sg_x, sg_y,
                               SG_W, SG_H, 0, CopyFromParent, InputOutput,
                               CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, sg_win, "SCDE Security");
    }
    sg_open = 1;
    sg_hover = -1;
    sg_hover_close = 0;
    XMapRaised(sc.dpy, sg_win);
    XSetInputFocus(sc.dpy, sg_win, RevertToPointerRoot, CurrentTime);
    XFlush(sc.dpy);
    sg_draw();
}

void security_gui_toggle(void)
{
    if (sg_open)
        security_gui_hide();
    else
        security_gui_show();
}

void security_gui_hide(void)
{
    if (!sg_open)
        return;
    sg_open = 0;
    XUnmapWindow(sc.dpy, sg_win);
    XFlush(sc.dpy);
}

int security_gui_open(void)
{
    return sg_open;
}

Window security_gui_window(void)
{
    return sg_win;
}

void security_gui_redraw(void)
{
    if (sg_open)
        sg_draw();
}

void security_gui_cleanup(void)
{
    if (sg_win != None) {
        XDestroyWindow(sc.dpy, sg_win);
        sg_win = None;
    }
    sg_open = 0;
}

/* --------------------------------------------------------------- events -- */

int security_gui_expose(XExposeEvent *ev)
{
    if (!sg_win || ev->window != sg_win)
        return 0;
    if (ev->count == 0)
        sg_draw();
    return 1;
}

int security_gui_motion(XMotionEvent *ev)
{
    int i, found = -1;
    int hc = 0;

    if (!sg_open || ev->window != sg_win)
        return 0;
    if (ev->y < SG_TITLE && ev->x >= SG_W - SG_TITLE)
        hc = 1;
    for (i = 0; i < sg_nbtns; i++) {
        if (ev->x >= sg_btns[i].x && ev->x < sg_btns[i].x + sg_btns[i].w &&
            ev->y >= sg_btns[i].y && ev->y < sg_btns[i].y + sg_btns[i].h) {
            found = i;
            break;
        }
    }
    if (found != sg_hover || hc != sg_hover_close) {
        sg_hover = found;
        sg_hover_close = hc;
        sg_draw();
    }
    return 1;
}

int security_gui_button(XButtonEvent *ev)
{
    int i;

    if (!sg_open || ev->window != sg_win)
        return 0;
    if (ev->button == Button4 || ev->button == Button5) {
        int n = scde_audit_count();

        if (ev->button == Button4 && sg_log_off + SG_LOG_ROWS < n)
            sg_log_off++;
        else if (ev->button == Button5 && sg_log_off > 0)
            sg_log_off--;
        sg_draw();
        return 1;
    }
    if (ev->button != Button1)
        return 1;
    XSetInputFocus(sc.dpy, sg_win, RevertToPointerRoot, CurrentTime);
    if (ev->y < SG_TITLE) {
        if (ev->x >= SG_W - SG_TITLE)
            security_gui_hide();
        else                            /* title bar drags the window */
            selfdrag_window(sg_win, ev, &sg_x, &sg_y, SG_W, SG_H);
        return 1;
    }
    for (i = 0; i < sg_nbtns; i++) {
        SecBtn *b = &sg_btns[i];

        if (ev->x < b->x || ev->x >= b->x + b->w ||
            ev->y < b->y || ev->y >= b->y + b->h)
            continue;
        if (b->id >= 10 && b->id <= 12)
            sg_set_mode(b->id);
        else if (b->id >= 20 && b->id <= 22)
            sg_run_scan(b->id);
        else if (b->id == 30)
            sg_toggle_bait();
        else if (b->id == 31)
            sg_toggle_mirage();
        else if (b->id == 32) {
            scde_bait_check();          /* manual detection sweep */
            sg_draw();
        } else if (b->id >= 100 && b->id < 100 + SCDE_POL_COUNT) {
            int key = b->id - 100;

            scde_policy_set(key, scde_policy_get(key) ? 0 : 1);
            sg_draw();
        } else if (b->id == 200) {
            scde_audit_clear();
            sg_log_off = 0;
            sg_draw();
        }
        return 1;
    }
    return 1;
}

void security_gui_key(XKeyEvent *ev)
{
    char tmp[8];
    KeySym sym = NoSymbol;

    if (!sg_open)
        return;
    XLookupString(ev, tmp, sizeof(tmp) - 1, &sym, NULL);
    if (sym == XK_Escape || sym == XK_q)
        security_gui_hide();
}
