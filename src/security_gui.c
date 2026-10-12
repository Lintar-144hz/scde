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

#define SG_W 600
#define SG_H 640
#define SG_TITLE 24
#define SG_PAD 10
#define SG_LOG_Y 522
#define SG_LOG_ROW 16
#define SG_LOG_ROWS 6
#define SG_SCAN_CAP 32
#define SG_MAX_BTNS 20

/* muted Harbor status tones: sage / amber / clay, never neon */
#define SG_OK   0x7fb08aUL
#define SG_WARN 0xc9a86aUL
#define SG_BAD  0xc07a7aUL

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
    /* Harbor: quiet hairline on every button, soft white on active */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sg_win, sc.gc, b->x, b->y, (unsigned)b->w,
                   (unsigned)b->h);
    draw_text(sg_win, cx - text_width(b->label) / 2,
              b->y + (b->h + sc.font_ascent - sc.font_descent) / 2,
              active ? 0xedf0f5UL : cfg.col_menu_fg, b->label);
}

/* Harbor card: menu-tone surface on the panel-tone window, hairline edge */
static void sg_card(int y, int h)
{
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, SG_PAD, y,
                   (unsigned)(SG_W - 2 * SG_PAD), (unsigned)h);
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sg_win, sc.gc, SG_PAD, y,
                   (unsigned)(SG_W - 2 * SG_PAD - 1), (unsigned)(h - 1));
}

static void sg_section(int top, const char *title)
{
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    draw_text(sg_win, SG_PAD + 10, top + sc.font_ascent,
              cfg.col_title_fg_inactive, title);
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
    /* Harbor card stack: quiet two-tone surfaces, generous breathing room */
    const int prot_y = 34, prot_h = 132;
    const int det_y = 174, det_h = 78;
    const int scan_y = 260, scan_h = 80;
    const int perm_y = 348, perm_h = 134;
    const int log_y = 490, log_h = 140;
    const int cx0 = SG_PAD + 10;
    const int cx1 = SG_W - SG_PAD - 10;

    if (!sg_win || !sg_open)
        return;
    sg_nbtns = 0;

    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, 0, 0, SG_W, SG_H);

    /* macOS-style title: calm close dot left, shield mark, title after */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, sg_win, sc.gc, 0, 0, SG_W, SG_TITLE);
    {
        int d = 12, ox = 6, oy = (SG_TITLE - d) / 2;

        XSetForeground(sc.dpy, sc.gc, 0xc07a7aUL);
        XFillArc(sc.dpy, sg_win, sc.gc, ox, oy, (unsigned)d, (unsigned)d, 0,
                 360 * 64);
        if (sg_hover_close) {
            XSetForeground(sc.dpy, sc.gc, 0xedf0f5UL);
            XDrawArc(sc.dpy, sg_win, sc.gc, ox - 2, oy - 2, (unsigned)(d + 4),
                     (unsigned)(d + 4), 0, 360 * 64);
        }
        XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
        XDrawLine(sc.dpy, sg_win, sc.gc, ox + 4, oy + 4, ox + 8, oy + 8);
        XDrawLine(sc.dpy, sg_win, sc.gc, ox + 4, oy + 8, ox + 8, oy + 4);
    }
    {
        XPoint p[5];
        int sx = 28, cy = SG_TITLE / 2;

        p[0].x = sx;
        p[0].y = cy - 7;
        p[1].x = sx + 13;
        p[1].y = cy - 7;
        p[2].x = sx + 13;
        p[2].y = cy;
        p[3].x = sx + 6;
        p[3].y = cy + 7;
        p[4].x = sx;
        p[4].y = cy;
        XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
        XFillPolygon(sc.dpy, sg_win, sc.gc, p, 5, Convex, CoordModeOrigin);
    }
    draw_text(sg_win, 48, 16, cfg.col_title_fg, "SCDE Security");
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XFillRectangle(sc.dpy, sg_win, sc.gc, 0, SG_TITLE - 1, SG_W, 1);

    mode = scde_security_get_scan_mode();
    watch_on = scde_security_watch_fd() >= 0;

    /* protection card: segmented mode + live status lines */
    sg_card(prot_y, prot_h);
    sg_section(prot_y + 4, "Protection");
    {
        /* status pill, top right of the card: calm dot + mode + watcher */
        unsigned long mc = mode == SCDE_SCAN_ALWAYS_ON ? SG_OK
                         : mode == SCDE_SCAN_PASSIVE  ? SG_WARN
                                                      : cfg.col_title_fg_inactive;
        char pill[96];
        int pw;

        snprintf(pill, sizeof(pill), "%s  ·  watcher %s",
                 mode_names[(int)mode], watch_on ? "active" : "off");
        pw = text_width(pill) + 14;
        XSetForeground(sc.dpy, sc.gc, mc);
        XFillRectangle(sc.dpy, sg_win, sc.gc, cx1 - pw - 10,
                       prot_y + 8, 6, 6);
        draw_text(sg_win, cx1 - pw, prot_y + 8 + sc.font_ascent, mc,
                  pill);
    }
    sg_btn(cx0, prot_y + 28, 110, 22, 10, "Always-on");
    sg_btn(cx0 + 118, prot_y + 28, 110, 22, 11, "Passive");
    sg_btn(cx0 + 236, prot_y + 28, 110, 22, 12, "Off");
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 10 && sg_btns[i].id <= 12)
            sg_paint_btn(&sg_btns[i], (sg_btns[i].id - 10) == (int)mode,
                         sg_hover == i);

    scde_security_stats(&st);
    snprintf(buf, sizeof(buf),
             "scans %u    threats %u    blocked %u    denied %u    "
             "rejected plugins %u",
             (unsigned)st.scans, (unsigned)st.threats,
             (unsigned)st.blocked, (unsigned)st.denied,
             (unsigned)st.rejected_plugins);
    draw_text(sg_win, cx0, prot_y + 66, cfg.col_menu_fg, buf);
    scde_security_db_info(&rules, &hashes, &trusted);
    snprintf(buf, sizeof(buf), "rules %d    bad hashes %d    trusted %d",
             rules, hashes, trusted);
    draw_text(sg_win, cx0, prot_y + 82, cfg.col_menu_fg, buf);

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
        fit_text(buf, sizeof(buf), buf, cx1 - cx0);
        draw_text(sg_win, cx0, prot_y + 98, cfg.col_title_fg_inactive,
                  buf);
    }
    {
        char lim[96];

        scde_resource_describe(lim, sizeof(lim));
        fit_text(buf, sizeof(buf), lim, cx1 - cx0);
        draw_text(sg_win, cx0, prot_y + 114, cfg.col_title_fg_inactive,
                  buf);
    }

    /* detection card (advisory only: bait/mirage detect, never isolate) */
    sg_card(det_y, det_h);
    sg_section(det_y + 4, "Detection (advisory, not a sandbox)");
    {
        int bait_on = scde_bait_enabled();
        int mir_on = scde_mirage_enabled();

        snprintf(buf, sizeof(buf), "Bait: %s  hits: %d",
                 !scde_bait_active() ? "OFF" : bait_on ? "ON" : "OFF",
                 scde_bait_hits());
        draw_text(sg_win, cx0, det_y + 44, cfg.col_menu_fg, buf);
        sg_btn(cx1 - 118, det_y + 30, 56, 16, 30,
               bait_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], bait_on,
                     sg_hover == sg_nbtns - 1);
        sg_btn(cx1 - 56, det_y + 30, 56, 16, 32, "Check");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], 0,
                     sg_hover == sg_nbtns - 1);

        snprintf(buf, sizeof(buf), "Mirage: %s  probes: %d",
                 !scde_mirage_active() ? "OFF" : mir_on ? "ON" : "OFF",
                 scde_mirage_probes());
        draw_text(sg_win, cx0, det_y + 66, cfg.col_menu_fg, buf);
        sg_btn(cx1 - 118, det_y + 52, 56, 16, 31,
               mir_on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], mir_on,
                     sg_hover == sg_nbtns - 1);
    }

    /* scanner card */
    sg_card(scan_y, scan_h);
    sg_section(scan_y + 4, "Scanner");
    sg_btn(cx0, scan_y + 28, 118, 22, 20, "Scan plugins");
    sg_btn(cx0 + 126, scan_y + 28, 118, 22, 21, "Scan Desktop");
    sg_btn(cx0 + 252, scan_y + 28, 118, 22, 22, "Scan Home");
    for (i = 0; i < sg_nbtns; i++)
        if (sg_btns[i].id >= 20 && sg_btns[i].id <= 22)
            sg_paint_btn(&sg_btns[i], 0, sg_hover == i);
    fit_text(buf, sizeof(buf), sg_scan_status, cx1 - cx0);
    draw_text(sg_win, cx0, scan_y + 68, cfg.col_menu_fg, buf);

    /* permissions card */
    sg_card(perm_y, perm_h);
    sg_section(perm_y + 4, "Permissions");
    for (i = 0; i < SCDE_POL_COUNT; i++) {
        int y = perm_y + 28 + i * 20;
        int on = scde_policy_get(i);

        draw_text(sg_win, cx0, y + 14, cfg.col_menu_fg, pol_names[i]);
        sg_btn(cx1 - 56, y + 2, 56, 16, 100 + i,
               on ? "ON" : "OFF");
        sg_paint_btn(&sg_btns[sg_nbtns - 1], on, sg_hover == sg_nbtns - 1);
    }

    /* log card: calm rows, clay tone only when risk is high */
    sg_card(log_y, log_h);
    sg_section(log_y + 4, "Log");
    sg_btn(cx1 - 60, log_y + 8, 60, 18, 200, "Clear");
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
            draw_text(sg_win, cx0, SG_LOG_Y + 12,
                      cfg.col_title_fg_inactive, "audit log is empty");
        for (i = start; i < end; i++) {
            scde_audit_entry_t e;
            int row = SG_LOG_Y + (i - start) * SG_LOG_ROW;
            unsigned long age;
            unsigned long color;
            char line[196];

            if (scde_audit_get(i, &e) != SCDE_OK)
                continue;
            age = (unsigned long)((now - e.time_ms) / 1000u);
            color = e.risk >= 50 ? SG_BAD : cfg.col_menu_fg;
            snprintf(line, sizeof(line), "%s %lu s  %s  r%d  %s",
                     sg_prefix(e.event), age,
                     scde_audit_event_name(e.event), e.risk, e.subject);
            fit_text(buf, sizeof(buf), line, cx1 - cx0);
            draw_text(sg_win, cx0, row + 12, color, buf);
        }
    }

    /* quiet outer hairline */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
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
        wa.background_pixel = cfg.col_panel_bg;
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
    focus_track(sg_win);
    taskbar_register(sg_win, TASK_APP_SECURITY, "SCDE Security");
    taskbar_set_hidden(sg_win, 0);
    panel_update();
    XFlush(sc.dpy);
    sg_draw();
}

void security_gui_toggle(void)
{
    if (sg_open)
        security_gui_close();
    else
        security_gui_show();
}

/*
 * Hide the window but keep its taskbar entry (minimise semantics): the
 * entry stays and a click on it brings the window back.
 */
void security_gui_hide(void)
{
    if (!sg_open)
        return;
    sg_open = 0;
    focus_drop(sg_win);
    /* focus falls back to the last focused client window, if any */
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    taskbar_register(sg_win, TASK_APP_SECURITY, "SCDE Security");
    taskbar_set_hidden(sg_win, 1);
    panel_update();
    XUnmapWindow(sc.dpy, sg_win);
    XFlush(sc.dpy);
}

/*
 * Close the window and drop its taskbar entry.  The X window object is
 * kept for a fast reopen; reopening registers exactly one fresh entry.
 */
void security_gui_close(void)
{
    if (sg_win == None)
        return;
    if (sg_open) {
        sg_open = 0;
        focus_drop(sg_win);
        XUnmapWindow(sc.dpy, sg_win);
        XFlush(sc.dpy);
    }
    taskbar_unregister(sg_win);
    panel_update();
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
        taskbar_unregister(sg_win);
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
    if (ev->y < SG_TITLE && ev->x < SG_TITLE)
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
    focus_track(sg_win);
    if (ev->y < SG_TITLE) {
        if (ev->x < SG_TITLE)
            security_gui_close();
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
        security_gui_close();
}
