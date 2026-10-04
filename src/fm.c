#define _XOPEN_SOURCE 700
/*
 * SCDE - built-in file manager (pure Xlib, no toolkit)
 *
 * Toolbar: Up / Back / Home / New Folder / Refresh + path bar.
 * List: icons, name, size, type.  Double-click opens directories and
 * files (images -> viewer, video/audio -> player, rest -> xdg-open).
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
#define FM_NAME_MAX 512
#define FM_PATH_MAX 1024
#define FM_MAX_BTNS 6

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
static int list_y0, list_y1, list_w, rows;
static int hover_btn, hover_close;

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
    char title[FM_PATH_MAX + 32];

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
}

static void fm_select(int idx)
{
    if (idx < 0)
        idx = 0;
    if (idx >= n_ents)
        idx = n_ents - 1;
    sel = idx;
    if (sel < scroll)
        scroll = sel;
    if (sel >= scroll + rows)
        scroll = sel - rows + 1;
    if (scroll > n_ents - rows)
        scroll = n_ents - rows;
    if (scroll < 0)
        scroll = 0;
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
    fm_reload();
    sel = 0;
    scroll = 0;
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
    fx = (sc.sw - fw) / 2;
    {
        int top = cfg.panel_top ? ph : 0;
        int bottom = cfg.panel_top ? sc.sh : sc.sh - ph;

        fy = top + (bottom - top - fh) / 2;
        if (fy < 0)
            fy = 0;
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
    tool_path_x = x + 4;
    tool_path_w = fw - tool_path_x - FM_PAD;
    if (tool_path_w < 40)
        tool_path_w = 40;

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
        XSetForeground(sc.dpy, sc.gc, C_FOLDER);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 1, iy - 3, 7, 3);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x, iy, 16, 11);
    } else {
        XSetForeground(sc.dpy, sc.gc, C_PAGE);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 1, iy - 2, 13, 16);
        XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 4, iy + 2, 7, 2);
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 4, iy + 6, 7, 2);
        XSetForeground(sc.dpy, sc.gc, kind_color(e));
        XFillRectangle(sc.dpy, fm_win, sc.gc, x + 8, iy + 9, 5, 4);
    }
}

static void fm_draw_button(int i, int hovered)
{
    static const char *const labels[FM_MAX_BTNS] = {
        "Up", "Back", "Home", "New Folder", "Refresh"
    };
    int by = FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 10)) / 2;
    int bh = sc.font_height + 10;
    int bl;

    XSetForeground(sc.dpy, sc.gc,
                   hovered ? cfg.col_btn_hover : cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, btn_x[i], by, (unsigned)btn_w[i],
                   (unsigned)bh);
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, fm_win, sc.gc, btn_x[i], by, (unsigned)btn_w[i] - 1,
                   (unsigned)bh - 1);
    bl = by + (bh + sc.font_ascent - sc.font_descent) / 2;
    draw_text(fm_win, btn_x[i] + 10, bl, cfg.col_menu_fg, labels[i]);
}

static void fm_draw_scrollbar(void)
{
    int track_h = list_y1 - list_y0;
    int max_scroll = n_ents - rows;
    int thumb_h, thumb_y;

    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, list_w, list_y0, (unsigned)FM_SB_W,
                   (unsigned)track_h);
    if (n_ents <= rows || max_scroll <= 0)
        return;
    thumb_h = track_h * rows / n_ents;
    if (thumb_h < 24)
        thumb_h = 24;
    thumb_y = list_y0 + (track_h - thumb_h) * scroll / max_scroll;
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_active);
    XFillRectangle(sc.dpy, fm_win, sc.gc, list_w + 3, thumb_y,
                   (unsigned)(FM_SB_W - 6), (unsigned)thumb_h);
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

    /* title bar */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, 0, (unsigned)fw,
                   (unsigned)FM_TITLE_H);
    bl = (FM_TITLE_H + sc.font_ascent - sc.font_descent) / 2;
    snprintf(buf, sizeof(buf), "Files  %s", cwd);
    draw_text_clip(fm_win, FM_PAD, fw - FM_TITLE_H - 6, bl,
                   cfg.col_title_fg, buf);
    XSetForeground(sc.dpy, sc.gc,
                   hover_close ? cfg.col_close_hover : cfg.col_title_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, fw - FM_TITLE_H + 1, 0,
                   (unsigned)(FM_TITLE_H - 2), (unsigned)(FM_TITLE_H - 1));
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_fg);
    XDrawLine(sc.dpy, fm_win, sc.gc, fw - FM_TITLE_H + 8,
              FM_TITLE_H / 2 - 4, fw - FM_TITLE_H + FM_TITLE_H - 9,
              FM_TITLE_H / 2 + 4);
    XDrawLine(sc.dpy, fm_win, sc.gc, fw - FM_TITLE_H + 8,
              FM_TITLE_H / 2 + 4, fw - FM_TITLE_H + FM_TITLE_H - 9,
              FM_TITLE_H / 2 - 4);

    /* toolbar */
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, FM_TITLE_H, (unsigned)fw,
                   (unsigned)FM_TOOL_H);
    for (i = 0; i < n_btn; i++)
        fm_draw_button(i, hover_btn == i);
    XSetForeground(sc.dpy, sc.gc, cfg.col_btn_inactive);
    XFillRectangle(sc.dpy, fm_win, sc.gc, tool_path_x,
                   FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2,
                   (unsigned)tool_path_w, (unsigned)(sc.font_height + 8));
    bl = FM_TITLE_H + (FM_TOOL_H - (sc.font_height + 8)) / 2 +
         (sc.font_height + 8 + sc.font_ascent - sc.font_descent) / 2;
    draw_text_clip(fm_win, tool_path_x + 6, tool_path_x + tool_path_w - 4,
                   bl, cfg.col_border_focus, cwd);

    /* list */
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, fm_win, sc.gc, 0, list_y0, (unsigned)list_w,
                   (unsigned)(list_y1 - list_y0));
    for (i = 0; i < rows; i++) {
        int idx = scroll + i;
        int y = list_y0 + i * rowh;
        int baseline;
        FmEntry *e;
        char sz[32];
        int type_x = list_w - 8 - 58;
        int sz_right = type_x - 12;

        if (idx >= n_ents)
            break;
        e = &ents[idx];
        if (idx == sel) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
            XFillRectangle(sc.dpy, fm_win, sc.gc, 0, y, (unsigned)list_w,
                           (unsigned)rowh);
        } else if (idx == hover) {
            XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
            XFillRectangle(sc.dpy, fm_win, sc.gc, 0, y, (unsigned)list_w,
                           (unsigned)rowh);
        }
        baseline = y + (rowh + sc.font_ascent - sc.font_descent) / 2;
        fm_draw_icon(8, baseline, e);
        {
            char shown[FM_NAME_MAX];
            int maxw = sz_right - 74 - 32;

            if (maxw < 40)
                maxw = 40;
            fit_text(shown, sizeof(shown), e->name, maxw);
            draw_text(fm_win, 32, baseline, cfg.col_menu_fg, shown);
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
            } else if (sel >= 0 && sel < n_ents) {
                if (ents[sel].is_dir)
                    snprintf(buf, sizeof(buf), "%d items   |   %s/",
                             n_ents, ents[sel].name);
                else {
                    fmt_size(sz, sizeof(sz), ents[sel].size);
                    snprintf(buf, sizeof(buf), "%d items   |   %s   %s   %s",
                             n_ents, ents[sel].name, kind_label(&ents[sel]),
                             sz);
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
    fm_select(sel);
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
                    fm_select(i);
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
                    fm_select(i);
                    break;
                }
            }
        }
    }
    mode = FM_MODE_NORMAL;
    fm_draw();
}

/* --------------------------------------------------------------- menus -- */

static void fm_ctx_show(int row, int x, int y)
{
    static const int dir_acts[] = { ACT_OPEN, ACT_RENAME, ACT_DELETE,
                                    ACT_PROPS, -1 };
    static const int file_acts[] = { ACT_OPEN, ACT_RENAME, ACT_DELETE,
                                     ACT_PROPS, ACT_WALL, -1 };
    static const int root_acts[] = { ACT_OPEN, ACT_PROPS, -1 };
    const int *acts;
    int i, ih = sc.font_height + 8;

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
    ctx_row = row;
    ctx_idx = 0;
    ctx_w = 190;
    ctx_h = ctx_n * ih + 6;
    ctx_x = x;
    ctx_y = y;
    if (ctx_x + ctx_w > fw - 2)
        ctx_x = fw - ctx_w - 2;
    if (ctx_y + ctx_h > list_y1 - 2)
        ctx_y = list_y1 - ctx_h - 2;
    if (ctx_x < 2)
        ctx_x = 2;
    if (ctx_y < list_y0 + 2)
        ctx_y = list_y0 + 2;
    mode = FM_MODE_CTX;
    fm_select(row);
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
        fm_select(sel);
        fm_draw();
        XMapRaised(sc.dpy, fm_win);
        XSetInputFocus(sc.dpy, fm_win, RevertToPointerRoot, CurrentTime);
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
    mode = FM_MODE_NORMAL;
    hover = hover_btn = hover_close = -1;
    last_click_i = -1;
    fm_set_name();
    fm_is_open = 1;
    XMapRaised(sc.dpy, fm_win);
    XSetInputFocus(sc.dpy, fm_win, RevertToPointerRoot, CurrentTime);
    XFlush(sc.dpy);
    fm_draw();
    log_msg("fm: opened %s (%d entries)", cwd, n_ents);
}

void fm_hide(void)
{
    if (!fm_is_open)
        return;
    fm_is_open = 0;
    mode = FM_MODE_NORMAL;
    XUnmapWindow(sc.dpy, fm_win);
    XFlush(sc.dpy);
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
    if (fm_win != None) {
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
    int max_scroll = n_ents - rows;

    if (n_ents <= rows || max_scroll <= 0)
        return 0;
    if (y < list_y0 || y > list_y1 || x < list_w || x >= fw)
        return 0;
    {
        int rel = y - list_y0;
        int new_s;

        new_s = rel * n_ents / track_h - rows / 2;
        if (new_s > max_scroll)
            new_s = max_scroll;
        if (new_s < 0)
            new_s = 0;
        if (new_s != scroll) {
            scroll = new_s;
            if (sel < scroll)
                sel = scroll;
            if (sel > scroll + rows - 1)
                sel = scroll + rows - 1;
            fm_draw();
        }
    }
    return 1;
}

int fm_button(XButtonEvent *ev)
{
    int in_fm;
    int row, y, i;

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

    /* title bar close */
    if (ev->y < FM_TITLE_H && ev->x >= fw - FM_TITLE_H) {
        fm_hide();
        return 1;
    }
    if (ev->y < FM_TITLE_H) {
        return 1;                         /* drag-less title bar */
    }

    /* toolbar buttons */
    if (ev->y >= FM_TITLE_H && ev->y < FM_TITLE_H + FM_TOOL_H) {
        int id = -1;

        for (i = 0; i < n_btn; i++) {
            if (ev->x >= btn_x[i] && ev->x < btn_x[i] + btn_w[i]) {
                id = i;
                break;
            }
        }
        if (id < 0)
            return 1;
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
                        fm_reload();
                        sel = scroll = 0;
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
            fm_select(sel);
            fm_draw();
        }
        return 1;
    }

    /* wheel scrolling over the list */
    if (ev->button == Button4 || ev->button == Button5) {
        if (ev->y >= list_y0 && ev->y < list_y1 && ev->x < list_w) {
            scroll += (ev->button == Button4) ? -3 : 3;
            if (scroll > n_ents - rows)
                scroll = n_ents - rows;
            if (scroll < 0)
                scroll = 0;
            if (sel < scroll)
                sel = scroll;
            if (sel > scroll + rows - 1)
                sel = scroll + rows - 1;
            fm_draw();
        }
        return 1;
    }

    /* scrollbar */
    if (ev->y >= list_y0 && ev->y < list_y1 && ev->x >= list_w) {
        fm_sb_click(ev->x, ev->y);
        return 1;
    }

    /* list rows */
    if (ev->y >= list_y0 && ev->y < list_y1 && ev->x < list_w) {
        y = ev->y - list_y0;
        row = scroll + y / fm_row_h();
        if (row < 0 || row >= n_ents)
            return 1;
        if (ev->button == Button3) {
            fm_ctx_show(row, ev->x + 4, ev->y + 4);
            return 1;
        }
        if (ev->button != Button1)
            return 1;
        {
            Time t = ev->time;

            if (row == last_click_i &&
                (t - last_click_t) < 500) {   /* double click */
                last_click_i = -1;
                fm_select(row);
                fm_open_idx(row);
                fm_draw();
                return 1;
            }
            last_click_t = t;
            last_click_i = row;
        }
        fm_select(row);
        fm_draw();
        XSetInputFocus(sc.dpy, fm_win, RevertToPointerRoot, CurrentTime);
        return 1;
    }

    /* status bar: clicking keeps focus for the edit line */
    if (ev->y >= list_y1) {
        XSetInputFocus(sc.dpy, fm_win, RevertToPointerRoot, CurrentTime);
        return 1;
    }
    return 1;
}

int fm_motion(XMotionEvent *ev)
{
    int nb = -1, nc = 0, nr = -1, new_hover;
    int x, y;

    if (!fm_win || ev->window != fm_win || !fm_is_open)
        return 0;
    x = ev->x;
    y = ev->y;

    if (y < FM_TITLE_H && x >= fw - FM_TITLE_H)
        nc = 1;
    if (y >= FM_TITLE_H && y < FM_TITLE_H + FM_TOOL_H) {
        int i;

        for (i = 0; i < n_btn; i++) {
            if (x >= btn_x[i] && x < btn_x[i] + btn_w[i]) {
                nb = i;
                break;
            }
        }
    } else if (y >= list_y0 && y < list_y1 && x < list_w) {
        int row = scroll + (y - list_y0) / fm_row_h();

        if (row >= 0 && row < n_ents)
            nr = row;
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
    new_hover = (nb < 0 && nc == 0) ? nr : -1;
    if (new_hover != hover || nb != hover_btn || nc != hover_close) {
        hover = new_hover;
        hover_btn = nb;
        hover_close = nc;
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

    switch (sym) {
    case XK_Escape:
        fm_hide();
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
        fm_select(sel - 1);
        fm_draw();
        break;
    case XK_Down:
        fm_select(sel + 1);
        fm_draw();
        break;
    case XK_Prior:
        fm_select(sel - rows);
        fm_draw();
        break;
    case XK_Next:
        fm_select(sel + rows);
        fm_draw();
        break;
    case XK_Home:
        fm_select(0);
        fm_draw();
        break;
    case XK_End:
        fm_select(n_ents - 1);
        fm_draw();
        break;
    case XK_F5:
        fm_reload();
        fm_select(sel);
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
            fm_select(sel);
            fm_draw();
        }
        break;
    default:
        break;
    }
}
