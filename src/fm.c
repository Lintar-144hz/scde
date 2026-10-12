#define _XOPEN_SOURCE 700
/*
 * SCDE - built-in file manager (pure Xlib, no toolkit)
 *
 * Harbor layout: sidebar places (Home, Desktop, Documents, ...), toolbar
 * with Up / Back / Home / New Folder / Refresh + path field + search,
 * readable list (icon, name, size, type).  Double-click opens directories
 * and files (images -> viewer, video/audio -> player, rest -> xdg-open).
 * Right-click: Open / Rename / Delete / Properties / Set as wallpaper.
 */
#include "scde.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FM_W        860
#define FM_H        600
#define FM_TITLE_H  26
#define FM_TOOL_H   36
#define FM_SB_W     14
#define FM_PAD      8
#define FM_SIDE_W   184
#define FM_SEARCH_W 150
#define FM_NAME_MAX 512
#define FM_PATH_MAX 1024
#define FM_MAX_BTNS 6
#define FM_PLACES   7

#define FM_MODE_NORMAL  0
#define FM_MODE_EDIT    1
#define FM_MODE_CONFIRM 2
#define FM_MODE_CTX     3

#define ACT_OPEN     0
#define ACT_RENAME   1
#define ACT_DELETE   2
#define ACT_PROPS    3
#define ACT_WALL     4

/* fixed accent colours for the file-type icons (RRGGBB) */
#define C_FOLDER  0xe3b04bUL
#define C_PAGE    0x9aa4b2UL
#define C_IMAGE   0x4da3ffUL
#define C_VIDEO   0xb478ffUL
#define C_AUDIO   0x4dd98aUL
#define C_TEXT    0xa8b3c4UL
#define C_ARCHIVE 0xd98a4dUL
#define C_HTML    0xff8a65UL
#define C_OTHER   0x6f7787UL

typedef struct {
    char name[FM_NAME_MAX];
    unsigned char is_dir;
    unsigned char kind;             /* F_KIND_* for files */
    unsigned long long size;
    time_t mtime;
} FmEntry;

static Window fm_win;
static int fm_is_open;
static int fx, fy, fw, fh;
static char cwd[FM_PATH_MAX];

static FmEntry *ents;
static int n_ents, ents_cap;
static int sel, scroll, hover;
static int mode, edit_what;
static char ibuf[FM_NAME_MAX];

static int n_btn;
static int btn_x[FM_MAX_BTNS], btn_w[FM_MAX_BTNS];
static int tool_path_x, tool_path_w;
static int search_x, search_w;
static int list_x0, list_y0, list_y1, list_w, rows;
static int hover_btn, hover_close, hover_side, hover_search;
static int search_focus;
static char sbuf[96];

/* visible-row map for the search filter (identity when idle) */
static int *vis_idx;
static int n_vis, vis_cap;

static int ctx_n, ctx_ids[8], ctx_idx;
static int ctx_x, ctx_y, ctx_w, ctx_h, ctx_target, ctx_row;

static Time last_click_t;
static int last_click_i;
static int show_hidden;

static char hist[24][FM_PATH_MAX];
static int n_hist;

enum { BTN_UP, BTN_BACK, BTN_HOME, BTN_NEW, BTN_REFRESH };

/* ------------------------------------------------------------ helpers -- */

static int fm_row_h(void)
{
    return sc.font_height + 10;
}

static void fm_join(char *out, size_t size, const char *dir, const char *name)
{
    size_t len = strlen(dir);

    if (len > 0 && dir[len - 1] == '/')
        snprintf(out, size, "%s%s", dir, name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

static const char *kind_label(const FmEntry *e)
{
    if (e->is_dir)
        return "Folder";
    switch (e->kind) {
    case F_KIND_IMAGE:   return "Image";
    case F_KIND_VIDEO:   return "Video";
    case F_KIND_AUDIO:   return "Audio";
    case F_KIND_TEXT:    return "Text";
    case F_KIND_ARCHIVE: return "Archive";
    case F_KIND_HTML:    return "Web";
    default:             return "File";
    }
}

static unsigned long kind_color(const FmEntry *e)
{
    if (e->is_dir)
        return C_FOLDER;
    switch (e->kind) {
    case F_KIND_IMAGE:   return C_IMAGE;
    case F_KIND_VIDEO:   return C_VIDEO;
    case F_KIND_AUDIO:   return C_AUDIO;
    case F_KIND_TEXT:    return C_TEXT;
    case F_KIND_ARCHIVE: return C_ARCHIVE;
    case F_KIND_HTML:    return C_HTML;
    default:             return C_OTHER;
    }
}

static void fmt_size(char *out, size_t size, unsigned long long bytes)
{
    if (bytes < 1024ULL)
        snprintf(out, size, "%llu B", bytes);
    else if (bytes < 1024ULL * 1024)
        snprintf(out, size, "%.1f kB", (double)bytes / 1024.0);
    else if (bytes < 1024ULL * 1024 * 1024)
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
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

/* ------------------------------------------------------ sidebar places -- */

static const char *const place_names[FM_PLACES] = {
    "Home", "Desktop", "Documents", "Downloads", "Music", "Pictures",
    "Videos"
};

/* index 0 resolves to $HOME itself, the rest to $HOME/<name> */
static void place_path(int i, char *out, size_t size)
{
    const char *home = getenv("HOME");

    if (!home || !*home)
        home = "/";
    if (i == 0)
        str_copy(out, size, home);
    else
        snprintf(out, size, "%s/%s", home, place_names[i]);
}

static int place_selected(int i)
{
    char p[FM_PATH_MAX];
    size_t len;

    place_path(i, p, sizeof(p));
    len = strlen(p);
    if (strcmp(cwd, p) == 0)
        return 1;
    if (i > 0 && strncmp(cwd, p, len) == 0 && cwd[len] == '/')
        return 1;
    if (i == 0 && cwd[0] != '\0') {
        int j;

        /* Home catches everything under $HOME not claimed above */
        for (j = 1; j < FM_PLACES; j++) {
            char q[FM_PATH_MAX];
            size_t ql;

            place_path(j, q, sizeof(q));
            ql = strlen(q);
            if (strcmp(cwd, q) == 0)
                return 0;
            if (strncmp(cwd, q, ql) == 0 && cwd[ql] == '/')
                return 0;
        }
        if (strncmp(cwd, p, len) == 0 && (cwd[len] == '/' || cwd[len] == '\0'))
            return 1;
    }
    return 0;
}

/* -------------------------------------------------------- search filter -- */

static int match_query_ci(const char *name, const char *q)
{
    size_t qlen;
    const char *p;

    if (!q || !*q)
        return 1;
    qlen = strlen(q);
    for (p = name; *p; p++) {
        size_t i;

        for (i = 0; i < qlen; i++) {
            char a = p[i], b = q[i];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a == '\0' || a != b)
                break;
        }
        if (i == qlen)
            return 1;
    }
    return 0;
}

/* rebuild the visible map after reload / query change; keeps selection
   only when the selected entry itself survives the filter */
static void fm_refilter(void)
{
    int i, keep = -1, found = 0;

    if (vis_cap < n_ents) {
        int ncap = n_ents > 0 ? n_ents : 128;
        int *p = realloc(vis_idx, (size_t)ncap * sizeof(int));

        if (!p)
            return;
        vis_idx = p;
        vis_cap = ncap;
    }
    n_vis = 0;
    for (i = 0; i < n_ents; i++) {
        int match = match_query_ci(ents[i].name, sbuf);

        if (i == sel) {
            keep = n_vis;
            found = match;
        }
        if (match)
            vis_idx[n_vis++] = i;
    }
    if (n_vis == 0)
        return;
    if (found && keep >= 0 && keep < n_vis) {
        /* selection survived: scroll it into view */
        if (keep < scroll)
            scroll = keep;
        if (keep >= scroll + rows)
            scroll = keep - rows + 1;
    } else {
        /* selected entry was filtered out (or the list was reloaded):
           fall back to the first visible entry */
        sel = vis_idx[0];
        scroll = 0;
    }
    if (scroll > n_vis - rows)
        scroll = n_vis - rows;
    if (scroll < 0)
        scroll = 0;
}

/* ------------------------------------------------------------- loading -- */

static int cmp_entry(const void *a, const void *b)
{
    const FmEntry *ea = a, *eb = b;

    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;
    return strcasecmp(ea->name, eb->name);
}

static void fm_set_name(void)
{
    char title[FM_PATH_MAX + 160];

    if (fm_win == None)
        return;
    /* keep the "scde-fm: <cwd>" prefix (tests match it); the filter
       suffix is a stable hook for humans and regression tests alike */
    if (sbuf[0] != '\0')
        snprintf(title, sizeof(title), "scde-fm: %s  [filter '%s': %d]",
                 cwd, sbuf, n_vis);
    else
        snprintf(title, sizeof(title), "scde-fm: %s", cwd);
    XStoreName(sc.dpy, fm_win, title);
}

static void fm_reload(void)
{
    DIR *d;
    struct dirent *de;

    n_ents = 0;
    d = opendir(cwd);
    if (!d) {
        log_msg("fm: cannot open %s: %s", cwd, strerror(errno));
        return;
    }
    while ((de = readdir(d)) != NULL) {
        FmEntry *e;
        char full[FM_PATH_MAX];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0)
            continue;
        if (de->d_name[0] == '.' && !show_hidden &&
            strcmp(de->d_name, "..") != 0)
            continue;
        if (strcmp(de->d_name, "..") == 0 && strcmp(cwd, "/") == 0)
            continue;
        if (n_ents >= ents_cap) {
            int ncap = ents_cap ? ents_cap * 2 : 128;
            FmEntry *p = realloc(ents, (size_t)ncap * sizeof(FmEntry));

            if (!p)
                break;
            ents = p;
            ents_cap = ncap;
        }
        e = &ents[n_ents];
        memset(e, 0, sizeof(*e));
        str_copy(e->name, sizeof(e->name), de->d_name);
        fm_join(full, sizeof(full), cwd, de->d_name);
        if (lstat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            e->size = (unsigned long long)st.st_size;
            e->mtime = st.st_mtime;
            if (!e->is_dir)
                e->kind = (unsigned char)apps_file_kind(full);
        }
        n_ents++;
    }
    closedir(d);

    /* ".." first, then sorted (dirs before files) */
    {
        int i, has_dotdot = -1;

        for (i = 0; i < n_ents; i++) {
            if (strcmp(ents[i].name, "..") == 0) {
                has_dotdot = i;
                break;
            }
        }
        if (has_dotdot > 0) {
            FmEntry tmp = ents[has_dotdot];

            memmove(&ents[1], &ents[0], (size_t)has_dotdot * sizeof(FmEntry));
            ents[0] = tmp;
        }
        if (has_dotdot >= 0 && n_ents > 1)
            qsort(ents + 1, (size_t)n_ents - 1, sizeof(FmEntry), cmp_entry);
        else if (n_ents > 0)
            qsort(ents, (size_t)n_ents, sizeof(FmEntry), cmp_entry);
    }
    if (sel >= n_ents)
        sel = n_ents - 1;
    if (sel < 0)
        sel = 0;
    if (scroll > n_ents)
        scroll = 0;
    fm_refilter();
    fm_set_name();
}

static int fm_vis_row_of_sel(void)
{
    int i;

    for (i = 0; i < n_vis; i++)
        if (vis_idx[i] == sel)
            return i;
    return -1;
}

/* idx is a visible row (identity when the filter is idle) */
static void fm_select(int idx)
{
    int span;

    /* filter with no matches: nothing to select, keep the view stable */
    if (n_vis == 0 && sbuf[0] != '\0')
        return;
    span = n_vis > 0 ? n_vis : n_ents;

    if (idx < 0)
        idx = 0;
    if (idx >= span)
        idx = span - 1;
    if (n_vis > 0)
        sel = vis_idx[idx];
    else
        sel = idx;
    if (idx < scroll)
        scroll = idx;
    if (idx >= scroll + rows)
        scroll = idx - rows + 1;
    if (scroll > span - rows)
        scroll = span - rows;
    if (scroll < 0)
        scroll = 0;
}

/* select by entry index, resolving through the visible map */
static void fm_select_ent(int eidx)
{
    int i;

    if (n_vis <= 0) {
        fm_select(eidx);
        return;
    }
    for (i = 0; i < n_vis; i++) {
        if (vis_idx[i] == eidx) {
            fm_select(i);
            return;
        }
    }
    fm_select(0);
}

static int fm_cd(const char *dir)
{
    struct stat st;
    char *real = realpath(dir, NULL);

    if (!real)
        return 0;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(real);
        return 0;
    }
    if (strcmp(real, cwd) != 0 && n_hist < (int)(sizeof(hist) / sizeof(hist[0])))
        str_copy(hist[n_hist++], sizeof(hist[0]), cwd);
    str_copy(cwd, sizeof(cwd), real);
    free(real);
    /* a new directory gets a fresh view: the filter belongs to its folder */
    sbuf[0] = '\0';
    search_focus = 0;
    fm_reload();
    sel = 0;
    scroll = 0;
    if (n_vis > 0)
        sel = vis_idx[0];
    fm_set_name();
    return 1;
}

static void fm_cd_parent(void)
{
    char parent[FM_PATH_MAX];
    char *slash;

    str_copy(parent, sizeof(parent), cwd);
    if (strlen(parent) > 1 && parent[strlen(parent) - 1] == '/')
        parent[strlen(parent) - 1] = '\0';
    slash = strrchr(parent, '/');
    if (!slash) {
        str_copy(parent, sizeof(parent), "/");
    } else if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    fm_cd(parent);
}

/* -------------------------------------------------------------- layout -- */

static void fm_layout(void)
{
    static const char *const labels[FM_MAX_BTNS] = {
        "Up", "Back", "Home", "New Folder", "Refresh"
    };
    static const int ids[FM_MAX_BTNS] = {
        BTN_UP, BTN_BACK, BTN_HOME, BTN_NEW, BTN_REFRESH
    };
    int i, x;
    int ph = panel_reserved();

    fw = FM_W;
    if (fw > sc.sw - 40)
        fw = sc.sw - 40;
    fh = FM_H;
    if (fh > sc.sh - ph - 40)
        fh = sc.sh - ph - 40;
    if (fh < 260)
        fh = 260;
    if (fm_win == None) {                /* only centre a fresh window;
                                            a dragged one keeps its spot */
        fx = (sc.sw - fw) / 2;
        {
            int top = cfg.panel_top ? ph : 0;
            int bottom = cfg.panel_top ? sc.sh : sc.sh - ph;

            fy = top + (bottom - top - fh) / 2;
            if (fy < 0)
                fy = 0;
        }
    }

    n_btn = 0;
    x = FM_PAD;
    for (i = 0; i < FM_MAX_BTNS; i++) {
        int w = text_width(labels[ids[i]]) + 20;

        if (w < 46)
            w = 46;
        btn_x[n_btn] = x;
        btn_w[n_btn] = w;
        n_btn++;
        x += w + 6;
    }
    /* Harbor toolbar: buttons left, path flexes, search docks right */
    search_w = FM_SEARCH_W;
    if (search_w > fw / 3)
        search_w = fw / 3;
    search_x = fw - FM_PAD - search_w;
    tool_path_x = x + 4;
    tool_path_w = search_x - 8 - tool_path_x;
    if (tool_path_w < 40)
        tool_path_w = 40;

    list_x0 = FM_SIDE_W;
    if (list_x0 > fw - FM_SB_W - 120)
        list_x0 = fw - FM_SB_W - 120;
    if (list_x0 < 0)
        list_x0 = 0;
    list_y0 = FM_TITLE_H + FM_TOOL_H;
    list_y1 = fh - (sc.font_height + 12);
    if (list_y1 < list_y0 + fm_row_h())
        list_y1 = list_y0 + fm_row_h();
    list_w = fw - FM_SB_W;
    rows = (list_y1 - list_y0) / fm_row_h();
    if (rows < 1)
        rows = 1;
}

/* --------------------------------------------------------------- paint -- */

static void fm_draw_icon(int x, int ycenter, const FmEntry *e)
{
    int h = sc.font_height;
    int iy = ycenter - 6;

    if (h < 14)
        iy = ycenter - 6;
    if (e->is_dir) {
        /* Harbor folder: tab + calm body, same 16x11 footprint */
        XSetForeground(sc.dpy, sc.gc, C_FOLDER);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 1, iy - 3, 7, 3);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x, iy, 16, 11);
        XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 1, iy + 1,
                       14, 1);
    } else {
        /* Harbor file: page with a folded corner + kind stripe */
        XSetForeground(sc.dpy, sc.gc, C_PAGE);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 1, iy - 2, 13, 16);
        XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 9, iy - 2, 5, 5);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 4, iy + 4, 7, 1);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 4, iy + 7, 7, 1);
        XSetForeground(sc.dpy, sc.gc, kind_color(e));
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 4, iy + 10, 7, 3);
    }
}

/* rounded chip painted onto the toolbar tone: cut corners read as a soft
   dock button without a compositor */
static void fm_chip(int x, int y, int wd, int hg, unsigned long fg)
{
    int cut = 3;

    if (wd <= 2 * cut || hg <= 2 * cut)
        return;
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, x + cut, y, (unsigned)(wd - 2 * cut),
                   (unsigned)hg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, x, y + cut, (unsigned)wd,
                   (unsigned)(hg - 2 * cut));
}

static void fm_draw_button(int i, int hovered)
{
    static const char *const labels[FM_MAX_BTNS] = {
        "Up", "Back", "Home", "New Folder", "Refresh"
    };
    int by = FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 10)) / 2;
    int bh = sc.font_height + 10;
    int bl;

    /* quiet chip: tone shift on hover, hairline only while hovered */
    fm_chip(btn_x[i], by, btn_w[i], bh,
            hovered ? cfg.col_btn_hover : cfg.col_menu_bg);
    if (hovered) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
        XDrawRectangle(sc.dpy, fm_win, sc.gc, btn_x[i], by,
                       (unsigned)(btn_w[i] - 1), (unsigned)(bh - 1));
    }
    bl = by + (bh + sc.font_ascent - sc.font_descent) / 2;
    draw_text(fm_win, btn_x[i] + 10, bl,
              hovered ? 0xedf0f5UL : cfg.col_menu_fg, labels[i]);
}

static void fm_draw_scrollbar(void)
{
    int track_h = list_y1 - list_y0;
    int span = n_vis > 0 ? n_vis : n_ents;
    int max_scroll = span - rows;
    int thumb_h, thumb_y;

    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, list_w, list_y0, (unsigned)FM_SB_W,
                   (unsigned)track_h);
    if (span <= rows || max_scroll <= 0)
        return;
    thumb_h = track_h * rows / span;
    if (thumb_h < 24)
        thumb_h = 24;
    thumb_y = list_y0 + (track_h - thumb_h) * scroll / max_scroll;
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_active);
    XFillRectangle(sc.dpy, fm_win, sc.gc, list_w + 3, thumb_y,
                   (unsigned)(FM_SB_W - 6), (unsigned)thumb_h);
}

/* Harbor search field: inset box, lens mark, placeholder or query + caret */
static void fm_draw_search(void)
{
    int by = FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2;
    int bh = sc.font_height + 8;
    int bl = by + (bh + sc.font_ascent - sc.font_descent) / 2;
    int tx = search_x + 22;

    XSetForeground(sc.dpy, sc.gc,
                   search_focus ? cfg.col_btn_hover : cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, search_x, by, (unsigned)search_w,
                   (unsigned)bh);
    XSetForeground(sc.dpy, sc.gc,
                   search_focus ? cfg.col_border_focus
                                : cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, fm_win, sc.gc, search_x, by,
                   (unsigned)(search_w - 1), (unsigned)(bh - 1));
    /* lens */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_fg_inactive);
    XDrawArc(sc.dpy, fm_win, sc.gc, search_x + 7, by + bh / 2 - 4, 7, 7, 0,
             360 * 64);
    XDrawLine(sc.dpy, fm_win, sc.gc, search_x + 12, by + bh / 2 + 2,
              search_x + 16, by + bh / 2 + 6);
    if (sbuf[0] != '\0') {
        int cx;

        draw_text_clip(fm_win, tx, search_x + search_w - 6, bl,
                       cfg.col_menu_fg, sbuf);
        if (search_focus) {
            cx = tx + text_width(sbuf) + 1;
            if (cx > search_x + search_w - 6)
                cx = search_x + search_w - 6;
            XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
            XFillRectangle(sc.dpy, fm_win, sc.gc, cx, bl - sc.font_ascent, 1,
                           (unsigned)(sc.font_ascent + sc.font_descent));
        }
    } else {
        draw_text_clip(fm_win, tx, search_x + search_w - 6, bl,
                       cfg.col_title_fg_inactive, "Search");
    }
}

static int fm_side_row_h(void)
{
    int h = sc.font_height + 12;

    return h < 26 ? 26 : h;
}

/* Harbor places sidebar: quiet two-tone column, selected pill, hover */
static void fm_draw_sidebar(void)
{
    int rh = fm_side_row_h();
    int y = list_y0 + 4;
    int i;

    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, list_y0, (unsigned)list_x0,
                   (unsigned)(list_y1 - list_y0));
    draw_text(fm_win, FM_PAD + 2, y + sc.font_ascent,
              cfg.col_title_fg_inactive, "Places");
    y += sc.font_height + 8;
    for (i = 0; i < FM_PLACES; i++) {
        int selrow = place_selected(i);
        int hov = hover_side == i;
        int bl = y + (rh + sc.font_ascent - sc.font_descent) / 2;

        if (selrow) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, fm_win, sc.gc, 4, y,
                           (unsigned)(list_x0 - 8), (unsigned)rh);
        } else if (hov) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
            XFillRectangle(sc.dpy, fm_win, sc.gc, 4, y,
                           (unsigned)(list_x0 - 8), (unsigned)rh);
        }
        /* folder tick */
        XSetForeground(sc.dpy, sc.gc,
                       selrow ? 0xedf0f5UL : C_FOLDER);
        XFillRectangle(sc.dpy, fm_win, sc.gc, FM_PAD + 4, bl - 8, 6, 2);
        XFillRectangle(sc.dpy, fm_win, sc.gc, FM_PAD + 3, bl - 6, 12, 8);
        draw_text_clip(fm_win, FM_PAD + 22, list_x0 - 8, bl,
                       selrow ? 0xedf0f5UL : cfg.col_menu_fg,
                       place_names[i]);
        y += rh;
        if (y + rh > list_y1)
            break;
    }
    /* separator between sidebar and list */
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawLine(sc.dpy, fm_win, sc.gc, list_x0, list_y0, list_x0, list_y1);
}

static void fm_draw_ctx(void)
{
    int i, y;
    int ih = sc.font_height + 8;
    static const char *const names[5] = {
        "Open", "Rename...", "Delete...", "Properties", "Set as wallpaper"
    };

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XFillRectangle(sc.dpy, fm_win, sc.gc, ctx_x, ctx_y, (unsigned)ctx_w,
                   (unsigned)ctx_h);
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, ctx_x + 1, ctx_y + 1,
                   (unsigned)(ctx_w - 2), (unsigned)(ctx_h - 2));
    for (i = 0; i < ctx_n; i++) {
        y = ctx_y + 3 + i * ih;
        if (i == ctx_idx) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, fm_win, sc.gc, ctx_x + 2, y,
                           (unsigned)(ctx_w - 4), (unsigned)ih);
        }
        draw_text(fm_win, ctx_x + 12,
                  y + (ih + sc.font_ascent - sc.font_descent) / 2,
                  cfg.col_menu_fg, names[ctx_ids[i]]);
    }
}

static void fm_draw(void)
{
    int bl, i;
    int rowh = fm_row_h();
    char buf[FM_PATH_MAX + 64];

    if (!fm_win || !fm_is_open)
        return;

    /* Harbor/macOS title: calm close dot on the left, centred title */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, 0, (unsigned)fw,
                   (unsigned)FM_TITLE_H);
    {
        int d = 12, ox = 8, oy = (FM_TITLE_H - d) / 2;

        XSetForeground(sc.dpy, sc.gc, 0xc07a7aUL);
        XFillArc(sc.dpy, fm_win, sc.gc, ox, oy, (unsigned)d, (unsigned)d, 0,
                 360 * 64);
        if (hover_close) {
            XSetForeground(sc.dpy, sc.gc, 0xedf0f5UL);
            XDrawArc(sc.dpy, fm_win, sc.gc, ox - 2, oy - 2, (unsigned)(d + 4),
                     (unsigned)(d + 4), 0, 360 * 64);
        }
        XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
        XDrawLine(sc.dpy, fm_win, sc.gc, ox + 4, oy + 4, ox + 8, oy + 8);
        XDrawLine(sc.dpy, fm_win, sc.gc, ox + 4, oy + 8, ox + 8, oy + 4);
    }
    bl = (FM_TITLE_H + sc.font_ascent - sc.font_descent) / 2;
    snprintf(buf, sizeof(buf), "Files  %s", cwd);
    {
        int left = FM_TITLE_H + 4;
        int avail = fw - FM_TITLE_H - 10 - left;
        char shown[256];

        if (avail > 40) {
            fit_text(shown, sizeof(shown), buf, avail);
            draw_text(fm_win, left + (avail - text_width(shown)) / 2, bl,
                      cfg.col_title_fg, shown);
        }
    }
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, FM_TITLE_H - 1, (unsigned)fw, 1);

    /* toolbar: buttons, path field, search field */
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, FM_TITLE_H, (unsigned)fw,
                   (unsigned)FM_TOOL_H);
    for (i = 0; i < n_btn; i++)
        fm_draw_button(i, hover_btn == i);
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, tool_path_x,
                   FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2,
                   (unsigned)tool_path_w, (unsigned)(sc.font_height + 8));
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, fm_win, sc.gc, tool_path_x,
                   FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2,
                   (unsigned)(tool_path_w - 1),
                   (unsigned)(sc.font_height + 7));
    bl = FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2 +
         (sc.font_height + 8 + sc.font_ascent - sc.font_descent) / 2;
    draw_text_clip(fm_win, tool_path_x + 6, tool_path_x + tool_path_w - 4,
                   bl, cfg.col_menu_fg, cwd);
    fm_draw_search();

    /* sidebar + list */
    fm_draw_sidebar();
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, list_x0, list_y0,
                   (unsigned)(list_w - list_x0),
                   (unsigned)(list_y1 - list_y0));
    for (i = 0; i < rows; i++) {
        int vrow = scroll + i;
        int idx;
        int y = list_y0 + i * rowh;
        int baseline;
        FmEntry *e;
        char sz[32];
        int type_x = list_w - 8 - 58;
        int sz_right = type_x - 12;
        int icon_x = list_x0 + 8;
        int text_x = list_x0 + 32;

        if (vrow >= n_vis)
            break;
        idx = vis_idx[vrow];
        if (idx < 0 || idx >= n_ents)
            continue;
        e = &ents[idx];
        if (idx == sel) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, fm_win, sc.gc, list_x0, y,
                           (unsigned)(list_w - list_x0), (unsigned)rowh);
        } else if (idx == hover) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
            XFillRectangle(sc.dpy, fm_win, sc.gc, list_x0, y,
                           (unsigned)(list_w - list_x0), (unsigned)rowh);
        }
        baseline = y + (rowh + sc.font_ascent - sc.font_descent) / 2;
        fm_draw_icon(icon_x, baseline, e);
        {
            char shown[FM_NAME_MAX];
            int maxw = sz_right - text_x - 42 - 32;

            if (maxw < 40)
                maxw = 40;
            fit_text(shown, sizeof(shown), e->name, maxw);
            draw_text(fm_win, text_x, baseline, cfg.col_menu_fg, shown);
        }
        if (e->is_dir) {
            sz[0] = '\0';
        } else {
            fmt_size(sz, sizeof(sz), e->size);
        }
        if (sz[0]) {
            int w = text_width(sz);

            draw_text(fm_win, sz_right - w, baseline,
                      idx == sel ? 0xe6ecf5UL : cfg.col_menu_fg, sz);
        }
        {
            int w = text_width(kind_label(e));

            draw_text(fm_win, list_w - 8 - w, baseline,
                      idx == sel ? 0xe6ecf5UL : cfg.col_menu_fg,
                      kind_label(e));
        }
    }
    if (sbuf[0] != '\0' && n_vis == 0)
        draw_text(fm_win, list_x0 + 12,
                  list_y0 + (rowh + sc.font_ascent - sc.font_descent) / 2,
                  cfg.col_title_fg_inactive, "No matches");
    fm_draw_scrollbar();

    /* status bar / input line */
    {
        int sy = list_y1;
        int sh = fh - list_y1;
        int sbl = sy + (sh + sc.font_ascent - sc.font_descent) / 2;

        XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
        XFillRectangle(sc.dpy, fm_win, sc.gc, 0, sy, (unsigned)fw,
                       (unsigned)sh);
        if (mode == FM_MODE_EDIT) {
            const char *pre = edit_what == 0 ? "Rename to:  " : "New folder: ";
            int px = FM_PAD + text_width(pre);
            int cw;

            draw_text(fm_win, FM_PAD, sbl, cfg.col_border_focus, pre);
            snprintf(buf, sizeof(buf), "%s", ibuf);
            draw_text_clip(fm_win, px, fw - FM_PAD - 4, sbl,
                           cfg.col_menu_fg, buf);
            cw = text_width(ibuf);
            XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
            XFillRectangle(sc.dpy, fm_win, sc.gc, px + cw,
                           sbl - sc.font_ascent, 2,
                           (unsigned)(sc.font_ascent + sc.font_descent));
        } else if (mode == FM_MODE_CONFIRM) {
            snprintf(buf, sizeof(buf), "Delete '%s'?  Press Y to confirm, Esc to cancel",
                     sel >= 0 && sel < n_ents ? ents[sel].name : "");
            draw_text_clip(fm_win, FM_PAD, fw - FM_PAD, sbl,
                           cfg.col_close_hover, buf);
        } else {
            char sz[32];

            if (n_ents == 0) {
                snprintf(buf, sizeof(buf), "Empty folder");
            } else if (sbuf[0] != '\0' && n_vis == 0) {
                snprintf(buf, sizeof(buf), "%d items   |   no matches for '%s'",
                         n_ents, sbuf);
            } else if (sel >= 0 && sel < n_ents) {
                if (sbuf[0] != '\0')
                    snprintf(buf, sizeof(buf), "%d of %d   |   ",
                             n_vis, n_ents);
                else
                    snprintf(buf, sizeof(buf), "%d items   |   ", n_ents);
                {
                    size_t used = strlen(buf);

                    if (ents[sel].is_dir)
                        snprintf(buf + used, sizeof(buf) - used, "%s/",
                                 ents[sel].name);
                    else {
                        fmt_size(sz, sizeof(sz), ents[sel].size);
                        snprintf(buf + used, sizeof(buf) - used,
                                 "%s   %s   %s", ents[sel].name,
                                 kind_label(&ents[sel]), sz);
                    }
                }
            } else {
                snprintf(buf, sizeof(buf), "%d items", n_ents);
            }
            draw_text_clip(fm_win, FM_PAD, fw - 200, sbl, cfg.col_menu_fg,
                           buf);
            if (show_hidden) {
                int w = text_width("hidden on (Ctrl+H)");

                draw_text(fm_win, fw - FM_PAD - w, sbl,
                          cfg.col_border_focus,
                          "hidden on (Ctrl+H)");
            }
        }
    }

    if (mode == FM_MODE_CTX)
        fm_draw_ctx();

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, fm_win, sc.gc, 0, 0, (unsigned)fw - 1,
                   (unsigned)fh - 1);
}

/* ------------------------------------------------------------ actions -- */

static void fm_props(void)
{
    char text[4096];
    char sz[32];
    char when[64];
    const FmEntry *e;

    if (sel < 0 || sel >= n_ents)
        return;
    e = &ents[sel];
    fmt_size(sz, sizeof(sz), e->size);
    if (e->mtime)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&e->mtime));
    else
        snprintf(when, sizeof(when), "-");
    snprintf(text, sizeof(text),
             "Name: %s\nType: %s\nSize: %s\nModified: %s\nPath:",
             e->name, e->is_dir ? "Folder" : kind_label(e),
             e->is_dir ? "-" : sz, when);
    {
        char full[FM_PATH_MAX + 80];

        fm_join(full, sizeof(full), cwd, e->name);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "\n%s",
                 full);
    }
    msgbox_show("Properties", text);
}

static void fm_set_wallpaper(void)
{
    char full[FM_PATH_MAX];
    const FmEntry *e;

    if (sel < 0 || sel >= n_ents)
        return;
    e = &ents[sel];
    if (e->is_dir || e->kind != F_KIND_IMAGE)
        return;
    if (!apps_have("feh")) {
        msgbox_show("No wallpaper support",
                    "Install feh to use images as wallpaper:\n"
                    "apt install feh");
        return;
    }
    fm_join(full, sizeof(full), cwd, e->name);
    if (config_set_wallpaper(full) != 0) {
        msgbox_show("Wallpaper not saved",
                    "Could not write wallpaper= into\n"
                    "~/.config/scde/config");
        return;
    }
    wallpaper_apply();
}

static int fm_rm_rf(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;

    if (!d)
        return unlink(path);
    while ((de = readdir(d)) != NULL) {
        char full[FM_PATH_MAX];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        fm_join(full, sizeof(full), path, de->d_name);
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (fm_rm_rf(full) != 0) {
                closedir(d);
                return -1;
            }
        } else if (unlink(full) != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

static void fm_delete_sel(void)
{
    char full[FM_PATH_MAX];

    if (sel < 0 || sel >= n_ents)
        return;
    if (strcmp(ents[sel].name, "..") == 0)
        return;
    fm_join(full, sizeof(full), cwd, ents[sel].name);
    if (fm_rm_rf(full) != 0) {
        char msg[1400];

        snprintf(msg, sizeof(msg), "Could not delete:\n%s\n%s", full,
                 strerror(errno));
        msgbox_show("Delete failed", msg);
    }
    fm_reload();
    fm_select_ent(sel);
    mode = FM_MODE_NORMAL;
    fm_draw();
}

static void fm_open_idx(int i)
{
    char full[FM_PATH_MAX];

    if (i < 0 || i >= n_ents)
        return;
    if (ents[i].is_dir) {
        if (strcmp(ents[i].name, "..") == 0) {
            fm_cd_parent();
            fm_draw();
            return;
        }
        fm_join(full, sizeof(full), cwd, ents[i].name);
        if (fm_cd(full)) {
            fm_draw();
        } else {
            msgbox_show("Cannot open folder", full);
        }
        return;
    }
    fm_join(full, sizeof(full), cwd, ents[i].name);
    apps_open_file(full);
}

static void fm_edit_commit(void)
{
    char from[FM_PATH_MAX], to[FM_PATH_MAX];
    char msg[512];

    if (ibuf[0] == '\0' || strchr(ibuf, '/') != NULL ||
        strcmp(ibuf, ".") == 0 || strcmp(ibuf, "..") == 0) {
        mode = FM_MODE_NORMAL;
        fm_draw();
        return;
    }
    if (edit_what == 0) {                       /* rename */
        if (sel < 0 || sel >= n_ents) {
            mode = FM_MODE_NORMAL;
            fm_draw();
            return;
        }
        fm_join(from, sizeof(from), cwd, ents[sel].name);
        fm_join(to, sizeof(to), cwd, ibuf);
        if (rename(from, to) != 0) {
            snprintf(msg, sizeof(msg), "Could not rename:\n%s", strerror(errno));
            msgbox_show("Rename failed", msg);
        } else {
            int i;

            fm_reload();
            for (i = 0; i < n_ents; i++) {
                if (strcmp(ents[i].name, ibuf) == 0) {
                    fm_select_ent(i);
                    break;
                }
            }
        }
    } else {                                    /* new folder */
        fm_join(to, sizeof(to), cwd, ibuf);
        if (mkdir(to, 0755) != 0 && errno != EEXIST) {
            snprintf(msg, sizeof(msg), "Could not create folder:\n%s",
                     strerror(errno));
            msgbox_show("New folder failed", msg);
        } else {
            int i;

            fm_reload();
            for (i = 0; i < n_ents; i++) {
                if (strcmp(ents[i].name, ibuf) == 0) {
                    fm_select_ent(i);
                    break;
                }
            }
        }
    }
    mode = FM_MODE_NORMAL;
    fm_draw();
}

/* --------------------------------------------------------------- menus -- */

static void fm_ctx_show(int vrow, int x, int y)
{
    static const int dir_acts[] = { ACT_OPEN, ACT_RENAME, ACT_DELETE,
                                    ACT_PROPS, -1 };
    static const int file_acts[] = { ACT_OPEN, ACT_RENAME, ACT_DELETE,
                                     ACT_PROPS, ACT_WALL, -1 };
    static const int root_acts[] = { ACT_OPEN, ACT_PROPS, -1 };
    const int *acts;
    int i, ih = sc.font_height + 8;
    int row;

    if (vrow < 0 || vrow >= n_vis)
        return;
    row = vis_idx[vrow];
    if (row < 0 || row >= n_ents)
        return;
    if (strcmp(ents[row].name, "..") == 0)
        acts = root_acts;
    else if (ents[row].is_dir)
        acts = dir_acts;
    else if (ents[row].kind == F_KIND_IMAGE)
        acts = file_acts;
    else {
        static int no_wall[5];

        no_wall[0] = ACT_OPEN;
        no_wall[1] = ACT_RENAME;
        no_wall[2] = ACT_DELETE;
        no_wall[3] = ACT_PROPS;
        no_wall[4] = -1;
        acts = no_wall;
    }
    ctx_n = 0;
    for (i = 0; acts[i] >= 0 && ctx_n < 8; i++)
        ctx_ids[ctx_n++] = acts[i];
    ctx_target = row;
    ctx_row = vrow;
    ctx_idx = 0;
    ctx_w = 190;
    ctx_h = ctx_n * ih + 6;
    ctx_x = x;
    ctx_y = y;
    if (ctx_x + ctx_w > fw - 2)
        ctx_x = fw - ctx_w - 2;
    if (ctx_y + ctx_h > list_y1 - 2)
        ctx_y = list_y1 - ctx_h - 2;
    if (ctx_x < list_x0 + 2)
        ctx_x = list_x0 + 2;
    if (ctx_y < list_y0 + 2)
        ctx_y = list_y0 + 2;
    mode = FM_MODE_CTX;
    fm_select(vrow);
    fm_draw();
}

static void fm_ctx_activate(void)
{
    int act;

    if (ctx_idx < 0 || ctx_idx >= ctx_n)
        return;
    act = ctx_ids[ctx_idx];
    sel = ctx_target;
    mode = FM_MODE_NORMAL;
    if (act == ACT_OPEN) {
        fm_open_idx(sel);
        fm_draw();
    } else if (act == ACT_RENAME && sel >= 0 && sel < n_ents &&
               strcmp(ents[sel].name, "..") != 0) {
        str_copy(ibuf, sizeof(ibuf), ents[sel].name);
        edit_what = 0;
        mode = FM_MODE_EDIT;
        fm_draw();
    } else if (act == ACT_DELETE) {
        mode = FM_MODE_CONFIRM;
        fm_draw();
    } else if (act == ACT_PROPS) {
        fm_draw();
        fm_props();
    } else if (act == ACT_WALL) {
        fm_draw();
        fm_set_wallpaper();
    } else {
        fm_draw();
    }
}

/* ---------------------------------------------------------- public API -- */

void fm_show(void)
{
    if (fm_is_open) {
        fm_reload();
        fm_select_ent(sel);
        fm_draw();
        XMapRaised(sc.dpy, fm_win);
        focus_track(fm_win);
        taskbar_register(fm_win, TASK_APP_FM, "File Manager");
        taskbar_set_hidden(fm_win, 0);
        panel_update();
        XFlush(sc.dpy);
        return;
    }

    fm_layout();

    if (fm_win == None) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_menu_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        KeyPressMask;
        fm_win = XCreateWindow(sc.dpy, sc.root, fx, fy, (unsigned)fw,
                               (unsigned)fh, 0, CopyFromParent, InputOutput,
                               CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
    } else {
        XMoveResizeWindow(sc.dpy, fm_win, fx, fy, (unsigned)fw,
                          (unsigned)fh);
    }

    if (cwd[0] == '\0') {
        const char *home = getenv("HOME");

        str_copy(cwd, sizeof(cwd), home && *home ? home : "/");
    }
    fm_reload();
    sel = 0;
    scroll = 0;
    if (n_vis > 0)
        sel = vis_idx[0];
    mode = FM_MODE_NORMAL;
    hover = hover_btn = hover_close = hover_side = hover_search = -1;
    search_focus = 0;
    last_click_i = -1;
    fm_set_name();
    fm_is_open = 1;
    XMapRaised(sc.dpy, fm_win);
    focus_track(fm_win);
    taskbar_register(fm_win, TASK_APP_FM, "File Manager");
    taskbar_set_hidden(fm_win, 0);
    panel_update();
    XFlush(sc.dpy);
    fm_draw();
    log_msg("fm: opened %s (%d entries)", cwd, n_ents);
}

void fm_show_path(const char *path)
{
    if (path && *path)
        fm_cd(path);
    fm_show();
}

/*
 * Hide the window but keep its taskbar entry (minimise semantics): the
 * entry stays and a click on it brings the window back.
 */
void fm_hide(void)
{
    if (!fm_is_open)
        return;
    fm_is_open = 0;
    mode = FM_MODE_NORMAL;
    focus_drop(fm_win);
    /* focus falls back to the last focused client window, if any */
    if (sc.focused && !sc.focused->minimized)
        focus_track(sc.focused->win);
    taskbar_register(fm_win, TASK_APP_FM, "File Manager");
    taskbar_set_hidden(fm_win, 1);
    panel_update();
    XUnmapWindow(sc.dpy, fm_win);
    XFlush(sc.dpy);
}

/*
 * Close the window and drop its taskbar entry.  The X window object is
 * kept for a fast reopen; reopening registers exactly one fresh entry.
 */
void fm_close(void)
{
    if (fm_win == None)
        return;
    if (fm_is_open) {
        fm_is_open = 0;
        mode = FM_MODE_NORMAL;
        focus_drop(fm_win);
        XUnmapWindow(sc.dpy, fm_win);
        XFlush(sc.dpy);
    }
    taskbar_unregister(fm_win);
    panel_update();
}

int fm_open(void)
{
    return fm_is_open;
}

Window fm_window(void)
{
    return fm_win;
}

void fm_cleanup(void)
{
    free(ents);
    ents = NULL;
    n_ents = ents_cap = 0;
    free(vis_idx);
    vis_idx = NULL;
    n_vis = vis_cap = 0;
    if (fm_win != None) {
        taskbar_unregister(fm_win);
        XDestroyWindow(sc.dpy, fm_win);
        fm_win = None;
    }
    fm_is_open = 0;
}

/* ------------------------------------------------------------- events -- */

int fm_expose(XExposeEvent *ev)
{
    if (!fm_win || ev->window != fm_win)
        return 0;
    if (ev->count == 0)
        fm_draw();
    return 1;
}

static int fm_sb_click(int x, int y)
{
    int track_h = list_y1 - list_y0;
    int span = n_vis > 0 ? n_vis : n_ents;
    int max_scroll = span - rows;

    if (span <= rows || max_scroll <= 0)
        return 0;
    if (y < list_y0 || y > list_y1 || x < list_w || x >= fw)
        return 0;
    {
        int rel = y - list_y0;
        int new_s;

        new_s = rel * span / track_h - rows / 2;
        if (new_s > max_scroll)
            new_s = max_scroll;
        if (new_s < 0)
            new_s = 0;
        if (new_s != scroll) {
            int vsel = fm_vis_row_of_sel();

            scroll = new_s;
            if (vsel >= 0) {
                if (vsel < scroll)
                    fm_select(scroll);
                if (vsel > scroll + rows - 1)
                    fm_select(scroll + rows - 1);
            }
            fm_draw();
        }
    }
    return 1;
}

/* sidebar hit test: -1 when the pointer is not on a place row */
static int fm_side_hit(int x, int y)
{
    int rh = fm_side_row_h();
    int top = list_y0 + 4 + sc.font_height + 8;
    int i;

    if (x < 0 || x >= list_x0 || y < top || y >= list_y1)
        return -1;
    i = (y - top) / rh;
    if (i < 0 || i >= FM_PLACES)
        return -1;
    return i;
}

int fm_button(XButtonEvent *ev)
{
    int in_fm;
    int y, i;

    if (!fm_win)
        return 0;
    in_fm = (ev->window == fm_win);

    if (!in_fm) {
        if (mode == FM_MODE_CTX) {   /* click outside closes the menu */
            mode = FM_MODE_NORMAL;
            fm_draw();
        }
        return 0;
    }
    if (ev->button == Button1)
        focus_track(fm_win);

    /* context menu open: only menu clicks count */
    if (mode == FM_MODE_CTX) {
        if (ev->x >= ctx_x && ev->x < ctx_x + ctx_w &&
            ev->y >= ctx_y && ev->y < ctx_y + ctx_h) {
            int ih = sc.font_height + 8;

            ctx_idx = (ev->y - ctx_y - 3) / ih;
            if (ctx_idx >= 0 && ctx_idx < ctx_n)
                fm_ctx_activate();
            return 1;
        }
        mode = FM_MODE_NORMAL;
        fm_draw();
        if (ev->button != Button1)
            return 1;
    }

    /* title bar close (left dot, macOS style) */
    if (ev->y < FM_TITLE_H && ev->x < FM_TITLE_H) {
        fm_close();
        return 1;
    }
    if (ev->y < FM_TITLE_H) {
        if (ev->button == Button1)   /* title bar drags the window */
            selfdrag_window(fm_win, ev, &fx, &fy, fw, fh);
        return 1;
    }

    /* toolbar: search field first, then buttons */
    if (ev->y >= FM_TITLE_H && ev->y < FM_TITLE_H + FM_TOOL_H) {
        int id = -1;

        if (ev->button == Button1 && ev->x >= search_x &&
            ev->x < search_x + search_w) {
            search_focus = 1;
            fm_draw();
            return 1;
        }
        search_focus = 0;
        for (i = 0; i < n_btn; i++) {
            if (ev->x >= btn_x[i] && ev->x < btn_x[i] + btn_w[i]) {
                id = i;
                break;
            }
        }
        if (id < 0) {
            fm_draw();
            return 1;
        }
        if (id == BTN_UP) {
            fm_cd_parent();
            fm_draw();
        } else if (id == BTN_BACK) {
            if (n_hist > 0) {
                char back[FM_PATH_MAX];

                str_copy(back, sizeof(back), hist[--n_hist]);
                hist[n_hist][0] = '\0';
                {
                    char *r = realpath(back, NULL);

                    if (r) {
                        str_copy(cwd, sizeof(cwd), r);
                        free(r);
                        sbuf[0] = '\0';
                        search_focus = 0;
                        fm_reload();
                        sel = scroll = 0;
                        if (n_vis > 0)
                            sel = vis_idx[0];
                        fm_set_name();
                        fm_draw();
                    }
                }
            }
        } else if (id == BTN_HOME) {
            const char *home = getenv("HOME");

            if (fm_cd(home && *home ? home : "/"))
                fm_draw();
        } else if (id == BTN_NEW) {
            ibuf[0] = '\0';
            edit_what = 1;
            mode = FM_MODE_EDIT;
            fm_draw();
        } else if (id == BTN_REFRESH) {
            fm_reload();
            fm_select_ent(sel);
            fm_draw();
        }
        return 1;
    }

    /* wheel scrolling over the list */
    if (ev->button == Button4 || ev->button == Button5) {
        int span = n_vis > 0 ? n_vis : n_ents;

        if (ev->y >= list_y0 && ev->y < list_y1 && ev->x < list_w &&
            ev->x >= list_x0) {
            int vsel = fm_vis_row_of_sel();

            scroll += (ev->button == Button4) ? -3 : 3;
            if (scroll > span - rows)
                scroll = span - rows;
            if (scroll < 0)
                scroll = 0;
            if (vsel >= 0) {
                if (vsel < scroll)
                    fm_select(scroll);
                if (vsel > scroll + rows - 1)
                    fm_select(scroll + rows - 1);
            }
            fm_draw();
        }
        return 1;
    }

    /* scrollbar */
    if (ev->y >= list_y0 && ev->y < list_y1 && ev->x >= list_w) {
        fm_sb_click(ev->x, ev->y);
        return 1;
    }

    /* sidebar places */
    if (ev->y >= list_y0 && ev->y < list_y1 && ev->x < list_x0) {
        int p = fm_side_hit(ev->x, ev->y);

        if (ev->button == Button1 && p >= 0) {
            char dest[FM_PATH_MAX];

            place_path(p, dest, sizeof(dest));
            if (!fm_cd(dest)) {
                char msg[FM_PATH_MAX + 64];

                snprintf(msg, sizeof(msg), "Cannot open folder:\n%s",
                         dest);
                msgbox_show("Cannot open folder", msg);
            }
            fm_draw();
        }
        return 1;
    }

    /* list rows */
    if (ev->y >= list_y0 && ev->y < list_y1 && ev->x < list_w) {
        int vrow;
        int eidx;

        y = ev->y - list_y0;
        vrow = scroll + y / fm_row_h();
        if (vrow < 0 || vrow >= n_vis)
            return 1;
        eidx = vis_idx[vrow];
        if (ev->button == Button3) {
            fm_ctx_show(vrow, ev->x + 4, ev->y + 4);
            return 1;
        }
        if (ev->button != Button1)
            return 1;
        {
            Time t = ev->time;

            if (vrow == last_click_i &&
                (t - last_click_t) < 500) {   /* double click */
                last_click_i = -1;
                fm_select(vrow);
                fm_open_idx(eidx);
                fm_draw();
                return 1;
            }
            last_click_t = t;
            last_click_i = vrow;
        }
        fm_select(vrow);
        fm_draw();
        focus_track(fm_win);
        return 1;
    }

    /* status bar: clicking keeps focus for the edit line */
    if (ev->y >= list_y1) {
        focus_track(fm_win);
        return 1;
    }
    return 1;
}

int fm_motion(XMotionEvent *ev)
{
    int nb = -1, nc = 0, nr = -1, ns = -1, nse = 0, new_hover;
    int x, y;

    if (!fm_win || ev->window != fm_win || !fm_is_open)
        return 0;
    x = ev->x;
    y = ev->y;

    if (y < FM_TITLE_H && x < FM_TITLE_H)
        nc = 1;
    if (y >= FM_TITLE_H && y < FM_TITLE_H + FM_TOOL_H) {
        int i;

        for (i = 0; i < n_btn; i++) {
            if (x >= btn_x[i] && x < btn_x[i] + btn_w[i]) {
                nb = i;
                break;
            }
        }
        if (x >= search_x && x < search_x + search_w)
            nse = 1;
    } else if (y >= list_y0 && y < list_y1) {
        if (x < list_x0) {
            ns = fm_side_hit(x, y);
        } else if (x < list_w) {
            int vrow = scroll + (y - list_y0) / fm_row_h();

            if (vrow >= 0 && vrow < n_vis)
                nr = vis_idx[vrow];
        }
    }
    if (mode == FM_MODE_CTX && x >= ctx_x && x < ctx_x + ctx_w &&
        y >= ctx_y && y < ctx_y + ctx_h) {
        int ih = sc.font_height + 8;
        int idx = (y - ctx_y - 3) / ih;

        if (idx >= 0 && idx < ctx_n) {
            if (idx != ctx_idx) {
                ctx_idx = idx;
                fm_draw();
            }
            return 1;
        }
    }
    new_hover = (nb < 0 && nc == 0 && nse == 0 && ns < 0) ? nr : -1;
    if (new_hover != hover || nb != hover_btn || nc != hover_close ||
        ns != hover_side || nse != hover_search) {
        hover = new_hover;
        hover_btn = nb;
        hover_close = nc;
        hover_side = ns;
        hover_search = nse;
        fm_draw();
    }
    return 1;
}

void fm_key(XKeyEvent *ev)
{
    char buf[64];
    KeySym sym = NoSymbol;
    int len;
    unsigned int mod = strip_mods(ev->state);

    if (!fm_is_open)
        return;
    len = XLookupString(ev, buf, sizeof(buf) - 1, &sym, NULL);
    buf[len > 0 ? len : 0] = '\0';

    if (mode == FM_MODE_CTX) {
        if (sym == XK_Escape) {
            mode = FM_MODE_NORMAL;
            fm_draw();
        } else if (sym == XK_Up) {
            if (ctx_idx > 0) {
                ctx_idx--;
                fm_draw();
            }
        } else if (sym == XK_Down) {
            if (ctx_idx < ctx_n - 1) {
                ctx_idx++;
                fm_draw();
            }
        } else if (sym == XK_Return || sym == XK_KP_Enter) {
            fm_ctx_activate();
        }
        return;
    }

    if (mode == FM_MODE_EDIT) {
        if (sym == XK_Escape) {
            mode = FM_MODE_NORMAL;
            fm_draw();
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            fm_edit_commit();
            return;
        }
        if (sym == XK_BackSpace) {
            if (mod == ControlMask)
                ibuf[0] = '\0';
            else
                utf8_pop(ibuf);
            fm_draw();
            return;
        }
        if (sym == XK_u && mod == ControlMask) {
            ibuf[0] = '\0';
            fm_draw();
            return;
        }
        if (len > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f &&
            strlen(ibuf) + (size_t)len < sizeof(ibuf) - 1 &&
            mod != ControlMask && mod != Mod1Mask) {
            strcat(ibuf, buf);
            fm_draw();
        }
        return;
    }

    if (mode == FM_MODE_CONFIRM) {
        if (sym == XK_y || sym == XK_Y) {
            fm_delete_sel();
        } else {
            mode = FM_MODE_NORMAL;
            fm_draw();
        }
        return;
    }

    /* Ctrl+F focuses the search field from anywhere in normal mode */
    if (mode == FM_MODE_NORMAL && ((sym == XK_f || sym == XK_F) &&
                                   mod == ControlMask)) {
        search_focus = 1;
        fm_draw();
        return;
    }

    /* search field input: printable text filters, navigation still moves */
    if (mode == FM_MODE_NORMAL && search_focus && sym != XK_Tab &&
        sym != XK_ISO_Left_Tab) {
        if (sym == XK_Escape) {
            if (sbuf[0] != '\0') {
                sbuf[0] = '\0';
                search_focus = 0;
                fm_reload();
                fm_select_ent(sel);
                fm_set_name();
                fm_draw();
            } else {
                search_focus = 0;
                fm_close();
            }
            return;
        }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            if (n_vis > 0)
                fm_open_idx(sel);
            fm_draw();
            return;
        }
        if (sym == XK_BackSpace) {
            if (mod == ControlMask)
                sbuf[0] = '\0';
            else
                utf8_pop(sbuf);
            fm_reload();
            fm_set_name();
            fm_draw();
            return;
        }
        if (sym == XK_u && mod == ControlMask) {
            sbuf[0] = '\0';
            fm_reload();
            fm_set_name();
            fm_draw();
            return;
        }
        if (len > 0 && (unsigned char)buf[0] >= 0x20 && buf[0] != 0x7f &&
            strlen(sbuf) + (size_t)len < sizeof(sbuf) - 1 &&
            mod != ControlMask && mod != Mod1Mask) {
            strcat(sbuf, buf);
            fm_reload();
            fm_set_name();
            fm_draw();
            return;
        }
        /* navigation keys fall through with focus kept */
    }

    switch (sym) {
    case XK_Escape:
        fm_close();
        break;
    case XK_Return:
    case XK_KP_Enter:
        fm_open_idx(sel);
        fm_draw();
        break;
    case XK_BackSpace:
        fm_cd_parent();
        fm_draw();
        break;
    case XK_Up:
        fm_select(fm_vis_row_of_sel() - 1);
        fm_draw();
        break;
    case XK_Down:
        fm_select(fm_vis_row_of_sel() + 1);
        fm_draw();
        break;
    case XK_Prior:
        fm_select(fm_vis_row_of_sel() - rows);
        fm_draw();
        break;
    case XK_Next:
        fm_select(fm_vis_row_of_sel() + rows);
        fm_draw();
        break;
    case XK_Home:
        fm_select(0);
        fm_draw();
        break;
    case XK_End:
        fm_select((n_vis > 0 ? n_vis : n_ents) - 1);
        fm_draw();
        break;
    case XK_F5:
        fm_reload();
        fm_select_ent(sel);
        fm_draw();
        break;
    case XK_F2:
        if (sel >= 0 && sel < n_ents && strcmp(ents[sel].name, "..") != 0) {
            str_copy(ibuf, sizeof(ibuf), ents[sel].name);
            edit_what = 0;
            mode = FM_MODE_EDIT;
            fm_draw();
        }
        break;
    case XK_Delete:
        if (sel >= 0 && sel < n_ents && strcmp(ents[sel].name, "..") != 0) {
            mode = FM_MODE_CONFIRM;
            fm_draw();
        }
        break;
    case XK_n:
        if (mod == ControlMask) {
            ibuf[0] = '\0';
            edit_what = 1;
            mode = FM_MODE_EDIT;
            fm_draw();
        }
        break;
    case XK_h:
        if (mod == ControlMask) {
            show_hidden = !show_hidden;
            fm_reload();
            fm_select_ent(sel);
            fm_draw();
        }
        break;
    default:
        break;
    }
}
