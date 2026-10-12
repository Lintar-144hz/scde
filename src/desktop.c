/*
 * SCDE - desktop shortcuts (icons drawn on the root window)
 *
 * Single click selects, double click opens, right click shows a context
 * menu, dragging rearranges the icon (positions are kept in
 * ~/.local/share/scde/desktop.layout).  The icons live in ~/Desktop and
 * a *.desktop entry is opened through its Exec= line.  Drawing on the
 * root window keeps them below every client without extra X windows.
 */
#define _XOPEN_SOURCE 700
#include "scde.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DESK_MAX     128
#define DESK_ICON_W  92
#define DESK_GLYPH   40
#define DESK_GAP     10
#define DESK_PAD     12
#define DRAG_SLOP    5
#define DOUBLE_MS    400

/* accent colours for the shortcut glyphs (RRGGBB) */
#define C_FOLDER  0xe3b04bUL
#define C_PAGE    0x9aa4b2UL
#define C_IMAGE   0x4da3ffUL
#define C_VIDEO   0xb478ffUL
#define C_AUDIO   0x4dd98aUL
#define C_ARCHIVE 0xd98a4dUL
#define C_HTML    0xff8a65UL
#define C_APP     0x6f93bbUL
#define C_OTHER   0x6f7787UL

typedef struct {
    char path[512];
    char name[NAME_MAX_LEN];
    char exec[512];                         /* Exec= of a .desktop entry */
    int x, y;
    int is_dir;
    int kind;
    int is_shortcut;
    int is_app;                             /* installed application */
    int custom;                             /* position came from the layout */
    int placed;                             /* owns a grid slot */
    int hidden;                             /* no free slot, not drawn */
} DeskIcon;

static DeskIcon *icons;
static int nicons;
static int sel = -1;
static int hover = -1;
static int desk_busy;
static char desk_dir[512];
static char layout_path[640];
static Atom atom_root_pmap;
static int last_click_idx = -1;
static Time last_click_time;

/* ------------------------------------------------------------- geometry -- */

static int icon_h(void)
{
    int h = 6 + DESK_GLYPH + 4 + sc.font_height + 6;

    return h < 76 ? 76 : h;
}

static int top_limit(void)
{
    return cfg.panel_top ? panel_reserved() + DESK_PAD : DESK_PAD;
}

static int bottom_limit(void)
{
    int bottom = sc.sh - (cfg.panel_top ? 0 : panel_reserved());

    return bottom - DESK_PAD - icon_h();
}

static void clamp_pos(DeskIcon *ic)
{
    int lo = top_limit();

    if (ic->x < DESK_PAD)
        ic->x = DESK_PAD;
    if (ic->x > sc.sw - DESK_ICON_W - DESK_PAD)
        ic->x = sc.sw - DESK_ICON_W - DESK_PAD;
    if (ic->x < DESK_PAD)
        ic->x = DESK_PAD;
    if (ic->y < lo)
        ic->y = lo;
    if (ic->y > bottom_limit())
        ic->y = bottom_limit();
    if (ic->y < lo)
        ic->y = lo;
}

static int cell_taken(int x, int y, int skip)
{
    int i, ih = icon_h();

    for (i = 0; i < nicons; i++) {
        if (i == skip || !icons[i].placed)
            continue;
        if (x < icons[i].x + DESK_ICON_W && x + DESK_ICON_W > icons[i].x &&
            y < icons[i].y + ih && y + ih > icons[i].y)
            return 1;
    }
    return 0;
}

/* give every icon without a saved position a slot on the grid */
static void desk_place(void)
{
    int ih = icon_h();
    int step = ih + DESK_GAP;
    int rows = (bottom_limit() - top_limit() + DESK_GAP) / step;
    int cols = (sc.sw - 2 * DESK_PAD + DESK_GAP) / (DESK_ICON_W + DESK_GAP);
    int total, slot = 0, i;

    if (rows < 1)
        rows = 1;
    if (cols < 1)
        cols = 1;
    total = rows * cols;

    for (i = 0; i < nicons; i++) {
        int x = DESK_PAD, y = top_limit(), tries, found = 0;

        if (icons[i].custom || icons[i].is_app)
            continue;
        for (tries = 0; tries < total + 1; tries++) {
            int s = slot % total;

            x = DESK_PAD + (s / rows) * (DESK_ICON_W + DESK_GAP);
            y = top_limit() + (s % rows) * step;
            slot++;
            if (!cell_taken(x, y, i)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            x = DESK_PAD;
            y = top_limit();
        }
        icons[i].x = x;
        icons[i].y = y;
        icons[i].placed = 1;
    }
    for (i = 0; i < nicons; i++)
        if (!icons[i].is_app)
            clamp_pos(&icons[i]);
}

/*
 * Installed applications fill the grid from the top left, left half of
 * the screen only: the classic right side stays free for open work, and
 * a cell that is already taken is skipped.  When the left half is full
 * the remaining applications are hidden instead of being stacked on top
 * of something else.
 */
static void desk_place_apps(void)
{
    int ih = icon_h();
    int step = ih + DESK_GAP;
    int rows = (bottom_limit() - top_limit() + DESK_GAP) / step;
    int half = sc.sw / 2;
    int i;

    if (rows < 1)
        rows = 1;
    for (i = 0; i < nicons; i++) {
        DeskIcon *ic = &icons[i];
        int x, done = 0;

        if (!ic->is_app || ic->custom)
            continue;
        for (x = DESK_PAD; x + DESK_ICON_W <= half && !done;
             x += (DESK_ICON_W + DESK_GAP)) {
            int r;

            for (r = 0; r < rows; r++) {
                int y = top_limit() + r * step;

                if (!cell_taken(x, y, i)) {
                    ic->x = x;
                    ic->y = y;
                    ic->placed = 1;
                    done = 1;
                    break;
                }
            }
        }
        if (!done)
            ic->hidden = 1;
    }
}

/* --------------------------------------------------------------- layout -- */

static void make_parent_dirs(const char *file)
{
    char buf[640];
    char *p;

    snprintf(buf, sizeof(buf), "%s", file);
    p = strrchr(buf, '/');
    if (!p)
        return;
    *p = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void desk_load_layout(void)
{
    FILE *f = fopen(layout_path, "r");
    char line[700];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char path[512];
        int x, y, i;

        if (sscanf(line, "%d %d %511[^\n]", &x, &y, path) != 3)
            continue;
        for (i = 0; i < nicons; i++) {
            if (strcmp(icons[i].path, path) == 0) {
                icons[i].x = x;
                icons[i].y = y;
                icons[i].custom = 1;
                icons[i].placed = 1;
                break;
            }
        }
    }
    fclose(f);
}

static void desk_save_layout(void)
{
    FILE *f;
    int i;

    make_parent_dirs(layout_path);
    f = fopen(layout_path, "w");
    if (!f) {
        log_msg("desktop: cannot write %s: %s", layout_path, strerror(errno));
        return;
    }
    for (i = 0; i < nicons; i++)
        if (icons[i].custom)
            fprintf(f, "%d %d %s\n", icons[i].x, icons[i].y, icons[i].path);
    fclose(f);
}

/* ----------------------------------------------------------------- scan -- */

static unsigned long desk_kind_color(const DeskIcon *ic)
{
    if (ic->is_dir)
        return C_FOLDER;
    switch (ic->kind) {
    case F_KIND_IMAGE:   return C_IMAGE;
    case F_KIND_VIDEO:   return C_VIDEO;
    case F_KIND_AUDIO:   return C_AUDIO;
    case F_KIND_TEXT:    return C_PAGE;
    case F_KIND_ARCHIVE: return C_ARCHIVE;
    case F_KIND_HTML:    return C_HTML;
    default:             return C_OTHER;
    }
}

static void strip_field_codes(char *s)
{
    char *w = s, *r = s;

    while (*r) {
        if (*r == '%' && r[1]) {
            if (r[1] == '%') {
                *w++ = '%';
                r += 2;
                continue;
            }
            if (strchr("fFuUdDnNickvm", r[1]) != NULL) {
                if (w > s && w[-1] == ' ')
                    w--;
                r += 2;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static int desk_read_shortcut(DeskIcon *ic)
{
    FILE *f = fopen(ic->path, "r");
    char line[1024];
    int in_entry = 0, has_exec = 0, hidden = 0, bad_type = 0;
    int terminal = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        char *val;

        if (nl)
            *nl = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (line[0] == '[') {
            in_entry = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_entry)
            continue;
        val = strchr(line, '=');
        if (!val)
            continue;
        *val++ = '\0';
        if (strcmp(line, "Exec") == 0) {
            str_copy(ic->exec, sizeof(ic->exec), val);
            strip_field_codes(ic->exec);
            if (ic->exec[0])
                has_exec = 1;
        } else if (strcmp(line, "Name") == 0 && val[0]) {
            str_copy(ic->name, sizeof(ic->name), val);
        } else if (strcmp(line, "Type") == 0) {
            if (strcmp(val, "Application") != 0)
                bad_type = 1;
        } else if (strcmp(line, "Terminal") == 0) {
            terminal = strcmp(val, "true") == 0;
        } else if (strcmp(line, "NoDisplay") == 0 ||
                   strcmp(line, "Hidden") == 0) {
            if (strcmp(val, "true") == 0)
                hidden = 1;
        }
    }
    fclose(f);
    if (!has_exec || hidden || bad_type)
        return 0;
    if (terminal && apps_terminal()[0]) {
        char term[128];
        char joined[660];

        str_copy(term, sizeof(term), apps_terminal());
        snprintf(joined, sizeof(joined), "%s -e %s", term, ic->exec);
        str_copy(ic->exec, sizeof(ic->exec), joined);
    }
    return 1;
}

static int cmp_icon(const void *a, const void *b)
{
    return strcmp(((const DeskIcon *)a)->name, ((const DeskIcon *)b)->name);
}

static int has_desktop_suffix(const char *name)
{
    size_t len = strlen(name);

    return len > 8 && strcmp(name + len - 8, ".desktop") == 0;
}

/* is there already an icon (on the desktop itself) with this file name? */
static int have_basename(const char *base)
{
    int i;

    for (i = 0; i < nicons; i++) {
        const char *slash = strrchr(icons[i].path, '/');

        if (slash && strcmp(slash + 1, base) == 0)
            return 1;
    }
    return 0;
}

/* join dir + "/" + name into out, "" when it would not fit */
static void desk_path_join(char *out, size_t size, const char *dir,
                           const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    if (dl + 1 + nl + 1 > size) {
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1, name, nl + 1);
}

static void desk_scan_app_dir(const char *dir)
{
    DIR *d;
    struct dirent *de;

    d = opendir(dir);
    if (!d)
        return;
    while ((de = readdir(d)) != NULL && nicons < DESK_MAX) {
        DeskIcon *ic;
        char path[640];

        if (!has_desktop_suffix(de->d_name))
            continue;
        if (have_basename(de->d_name))
            continue;                   /* already lives in ~/Desktop */
        desk_path_join(path, sizeof(path), dir, de->d_name);
        if (!path[0])
            continue;
        ic = &icons[nicons];
        memset(ic, 0, sizeof(*ic));
        str_copy(ic->path, sizeof(ic->path), path);
        str_copy(ic->name, sizeof(ic->name), de->d_name);
        if (!desk_read_shortcut(ic))
            continue;
        ic->is_shortcut = 1;
        ic->is_app = 1;
        nicons++;
    }
    closedir(d);
}

/* add every installed application as a desktop icon (cfg.desktop_apps) */
static void desk_scan_apps(void)
{
    static const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    const char *home = getenv("HOME");
    char user_dir[640];
    int i;

    for (i = 0; dirs[i]; i++)
        desk_scan_app_dir(dirs[i]);
    if (home && *home) {
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications",
                 home);
        desk_scan_app_dir(user_dir);
    }
}

static void desk_scan(void)
{
    DIR *d;
    struct dirent *de;

    nicons = 0;
    sel = -1;
    hover = -1;
    last_click_idx = -1;

    if (icons == NULL) {
        icons = calloc(DESK_MAX, sizeof(DeskIcon));
        if (!icons)
            return;
    }
    d = opendir(desk_dir);
    if (!d)
        goto have_icons;                /* still show the installed apps */
    while ((de = readdir(d)) != NULL && nicons < DESK_MAX) {
        DeskIcon *ic = &icons[nicons];
        struct stat st;
        size_t len;

        if (de->d_name[0] == '.')
            continue;
        if (strlen(desk_dir) + 1 + strlen(de->d_name) >= sizeof(ic->path))
            continue;
        memset(ic, 0, sizeof(*ic));
        snprintf(ic->path, sizeof(ic->path), "%s/%s", desk_dir, de->d_name);
        str_copy(ic->name, sizeof(ic->name), de->d_name);
        if (lstat(ic->path, &st) != 0)
            continue;
        ic->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        if (!ic->is_dir)
            ic->kind = (unsigned char)apps_file_kind(ic->path);
        len = strlen(ic->name);
        if (!ic->is_dir && len > 8 &&
            strcmp(ic->name + len - 8, ".desktop") == 0) {
            if (desk_read_shortcut(ic))
                ic->is_shortcut = 1;
        }
        nicons++;
    }
    closedir(d);

have_icons:
    if (cfg.desktop_apps)
        desk_scan_apps();

    if (nicons > 1)
        qsort(icons, (size_t)nicons, sizeof(DeskIcon), cmp_icon);
    desk_load_layout();
    desk_place();
    desk_place_apps();
}

/* --------------------------------------------------------------- painting */

static void desk_clear(const DeskIcon *ic)
{
    XClearArea(sc.dpy, sc.root, ic->x, ic->y, DESK_ICON_W, icon_h(), False);
}

static void desk_clear_all(void)
{
    int top = top_limit() - DESK_GAP;

    if (top < 0)
        top = 0;
    XClearArea(sc.dpy, sc.root, 0, top, (unsigned)sc.sw,
               (unsigned)(sc.sh - top), False);
}

static void desk_draw_glyph(const DeskIcon *ic)
{
    int cx = ic->x + DESK_ICON_W / 2;
    int gy = ic->y + 6;
    int gw = 30;
    int gh = DESK_GLYPH - 8;
    int gx = cx - gw / 2;
    unsigned long col = desk_kind_color(ic);

    if (ic->is_dir) {
        XSetForeground(sc.dpy, sc.gc, C_FOLDER);
        XFillRectangle(sc.dpy, sc.root, sc.gc, gx + 1, gy + 4,
                       (unsigned)(gw / 2), 5);
        XFillRectangle(sc.dpy, sc.root, sc.gc, gx, gy + 7, (unsigned)gw,
                       (unsigned)(gh - 3));
        return;
    }
    if (ic->is_shortcut) {
        XPoint tri[3];

        XSetForeground(sc.dpy, sc.gc, C_APP);
        XFillRectangle(sc.dpy, sc.root, sc.gc, gx, gy, (unsigned)gw,
                       (unsigned)gh);
        tri[0].x = (short)(gx + 11);
        tri[0].y = (short)(gy + 7);
        tri[1].x = (short)(gx + 22);
        tri[1].y = (short)(gy + gh / 2);
        tri[2].x = (short)(gx + 11);
        tri[2].y = (short)(gy + gh - 7);
        XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
        XFillPolygon(sc.dpy, sc.root, sc.gc, tri, 3, Convex, CoordModeOrigin);
        return;
    }

    XSetForeground(sc.dpy, sc.gc, C_PAGE);
    XFillRectangle(sc.dpy, sc.root, sc.gc, gx + 3, gy, (unsigned)(gw - 6),
                   (unsigned)gh);
    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, sc.root, sc.gc, gx + gw - 11, gy, 8, 8);
    XFillRectangle(sc.dpy, sc.root, sc.gc, gx + 7, gy + 13, 13, 2);
    XFillRectangle(sc.dpy, sc.root, sc.gc, gx + 7, gy + 19, 13, 2);
    XSetForeground(sc.dpy, sc.gc, col);
    XFillRectangle(sc.dpy, sc.root, sc.gc, gx + 7, gy + gh - 9, 9, 5);
}

static void desk_draw_label(const DeskIcon *ic, int bright)
{
    char buf[NAME_MAX_LEN];
    int bl, tx, tw, bw;
    int y0 = ic->y + 6 + DESK_GLYPH + 4;

    fit_text(buf, sizeof(buf), ic->name, DESK_ICON_W - 8);
    if (!buf[0])
        return;
    tw = text_width(buf);
    bw = tw + 8;
    tx = ic->x + (DESK_ICON_W - bw) / 2;
    if (tx < ic->x)
        tx = ic->x;
    bl = y0 + (sc.font_height + sc.font_ascent - sc.font_descent) / 2;

    /* Harbor label pill: charcoal chip with a hairline, soft white text */
    XSetForeground(sc.dpy, sc.gc,
                   bright ? cfg.col_menu_sel : cfg.col_panel_bg);
    XFillRectangle(sc.dpy, sc.root, sc.gc, tx, y0 - 1, (unsigned)bw,
                   (unsigned)(sc.font_height + 3));
    XSetForeground(sc.dpy, sc.gc,
                   bright ? cfg.col_border_focus : cfg.col_border_unfocus);
    XDrawRectangle(sc.dpy, sc.root, sc.gc, tx, y0 - 1, (unsigned)(bw - 1),
                   (unsigned)(sc.font_height + 2));
    draw_text(sc.root, tx + 4, bl,
              bright ? 0xedf0f5UL : cfg.col_panel_fg, buf);
}

static void desk_paint_icon(int i)
{
    DeskIcon *ic = &icons[i];

    if (ic->hidden)
        return;
    if (i == sel) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_menu_sel);
        XFillRectangle(sc.dpy, sc.root, sc.gc, ic->x, ic->y,
                       DESK_ICON_W, (unsigned)icon_h());
        XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
        XDrawRectangle(sc.dpy, sc.root, sc.gc, ic->x, ic->y,
                       DESK_ICON_W - 1, (unsigned)(icon_h() - 1));
    } else if (i == hover) {
        XSetForeground(sc.dpy, sc.gc, cfg.col_btn_hover);
        XFillRectangle(sc.dpy, sc.root, sc.gc, ic->x, ic->y,
                       DESK_ICON_W, (unsigned)icon_h());
    }
    desk_draw_glyph(ic);
    desk_draw_label(ic, i == sel);
}

void desktop_paint(void)
{
    int i;

    if (!sc.dpy || nicons == 0)
        return;
    for (i = 0; i < nicons; i++)
        desk_paint_icon(i);
}

/*
 * Repaint only the desktop region (x, y, w, h): the background plus every
 * icon that intersects it.  Called after a window vacated an area (or a
 * root Expose arrived) so the icons under the old position come back
 * without repainting the whole screen.
 */
void desktop_damage(int x, int y, int w, int h)
{
    int i;

    if (!sc.dpy || w <= 0 || h <= 0)
        return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > sc.sw)
        w = sc.sw - x;
    if (y + h > sc.sh)
        h = sc.sh - y;
    if (w <= 0 || h <= 0)
        return;
    XClearArea(sc.dpy, sc.root, x, y, (unsigned)w, (unsigned)h, False);
    for (i = 0; i < nicons; i++) {
        DeskIcon *ic = &icons[i];

        if (ic->hidden)
            continue;
        if (ic->x < x + w && ic->x + DESK_ICON_W > x &&
            ic->y < y + h && ic->y + icon_h() > y)
            desk_paint_icon(i);
    }
}

/* --------------------------------------------------------------- actions */

void desktop_open(int i)
{
    if (i < 0 || i >= nicons)
        return;
    if (icons[i].is_shortcut) {
        spawn(icons[i].exec);
        log_msg("desktop: run '%s'", icons[i].exec);
        return;
    }
    if (icons[i].is_dir) {
        fm_show_path(icons[i].path);
        return;
    }
    apps_open_file(icons[i].path);
}

static void desk_parent(char *out, size_t size, const char *path)
{
    char *slash;

    str_copy(out, size, path);
    slash = strrchr(out, '/');
    if (!slash || slash == out)
        str_copy(out, size, "/");
    else
        *slash = '\0';
}

static void desk_open_in_fm(void)
{
    if (sel < 0 || sel >= nicons)
        return;
    if (icons[sel].is_dir)
        fm_show_path(icons[sel].path);
    else {
        char parent[512];

        desk_parent(parent, sizeof(parent), icons[sel].path);
        fm_show_path(parent);
    }
}

static void desk_props(void)
{
    struct stat st;
    char size_txt[48];
    char text[768];
    const char *kind;

    if (sel < 0 || sel >= nicons)
        return;
    kind = icons[sel].is_shortcut ? "Application shortcut"
           : icons[sel].is_dir    ? "Folder"
                                  : "File";
    if (stat(icons[sel].path, &st) != 0)
        memset(&st, 0, sizeof(st));
    if (icons[sel].is_dir)
        str_copy(size_txt, sizeof(size_txt), "-");
    else if (st.st_size >= 1048576)
        snprintf(size_txt, sizeof(size_txt), "%.1f MB",
                 (double)st.st_size / 1048576.0);
    else if (st.st_size >= 1024)
        snprintf(size_txt, sizeof(size_txt), "%.1f kB",
                 (double)st.st_size / 1024.0);
    else
        snprintf(size_txt, sizeof(size_txt), "%lld bytes",
                 (long long)st.st_size);
    snprintf(text, sizeof(text), "%s\n\n%s\n%s\n\nsize: %s",
             icons[sel].name, kind, icons[sel].path, size_txt);
    msgbox_show("Properties", text);
}

static void desk_set_wallpaper(void)
{
    if (sel < 0 || sel >= nicons)
        return;
    if (icons[sel].is_dir || icons[sel].kind != F_KIND_IMAGE) {
        msgbox_show("Not an image",
                    "Only image files can be used as a\n"
                    "desktop wallpaper.");
        return;
    }
    if (!apps_have("feh")) {
        msgbox_show("No wallpaper support",
                    "Install feh to use images as wallpaper:\n"
                    "apt install feh");
        return;
    }
    if (config_set_wallpaper(icons[sel].path) != 0) {
        msgbox_show("Wallpaper not saved",
                    "Could not write wallpaper= into\n"
                    "~/.config/scde/config");
        return;
    }
    wallpaper_apply();
}

static void desk_new_item(const char *base, int is_dir)
{
    char path[600];
    int i;

    mkdir(desk_dir, 0755);
    for (i = 1; i < 200; i++) {
        if (i == 1)
            snprintf(path, sizeof(path), "%s/%s", desk_dir, base);
        else
            snprintf(path, sizeof(path), "%s/%s %d", desk_dir, base, i);
        if (access(path, F_OK) != 0)
            break;
    }
    if (i >= 200)
        return;
    if (is_dir) {
        if (mkdir(path, 0755) != 0) {
            log_msg("desktop: mkdir %s: %s", path, strerror(errno));
            return;
        }
    } else {
        FILE *f = fopen(path, "w");

        if (!f) {
            log_msg("desktop: create %s: %s", path, strerror(errno));
            return;
        }
        fclose(f);
    }
    log_msg("desktop: created %s", path);
    desk_clear_all();
    desk_scan();
    desktop_paint();
    for (i = 0; i < nicons; i++) {
        if (strcmp(icons[i].path, path) == 0) {
            sel = i;
            desktop_paint();
            break;
        }
    }
}

int desktop_command(const char *cmd)
{
    if (!cmd || strncmp(cmd, "@d-", 3) != 0)
        return 0;
    if (strcmp(cmd, "@d-refresh") == 0)
        desktop_refresh();
    else if (strcmp(cmd, "@d-newdir") == 0)
        desk_new_item("New Folder", 1);
    else if (strcmp(cmd, "@d-newfile") == 0)
        desk_new_item("New File", 0);
    else if (strcmp(cmd, "@d-open") == 0)
        desktop_open(sel);
    else if (strcmp(cmd, "@d-openfm") == 0)
        desk_open_in_fm();
    else if (strcmp(cmd, "@d-props") == 0)
        desk_props();
    else if (strcmp(cmd, "@d-wall") == 0)
        desk_set_wallpaper();
    else
        return 0;
    return 1;
}

/* ---------------------------------------------------------------- input -- */

static int desk_hit(int x, int y)
{
    int i, ih = icon_h();

    for (i = 0; i < nicons; i++)
        if (!icons[i].hidden &&
            x >= icons[i].x && x < icons[i].x + DESK_ICON_W &&
            y >= icons[i].y && y < icons[i].y + ih)
            return i;
    return -1;
}

static void desk_select(int i)
{
    if (i == sel)
        return;
    if (sel >= 0 && sel < nicons)
        desk_clear(&icons[sel]);
    sel = i;
    desktop_paint();
}

static void desk_icon_menu(XButtonEvent *ev, int i)
{
    static MenuEntry items[8];
    int n = 0;

#define ADD(L, C) do { \
        str_copy(items[n].label, sizeof(items[0].label), (L)); \
        str_copy(items[n].cmd, sizeof(items[0].cmd), (C)); \
        n++; \
    } while (0)
    (void)i;
    ADD("Open", "@d-open");
    ADD("Open in File Manager", "@d-openfm");
    ADD("-", "");
    ADD("Properties", "@d-props");
    if (!icons[i].is_dir && icons[i].kind == F_KIND_IMAGE)
        ADD("Set as wallpaper", "@d-wall");
    ADD("-", "");
    ADD("Refresh", "@d-refresh");
#undef ADD

    menu_show_custom(items, n, ev->x_root, ev->y_root);
}

static void desk_drag(int idx, XButtonEvent *ev)
{
    XEvent e, last, pending;
    int ox = icons[idx].x, oy = icons[idx].y;
    int dx0 = ev->x_root - ox, dy0 = ev->y_root - oy;
    int moved = 0, has_pending = 0;

    if (XGrabPointer(sc.dpy, sc.root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, sc.cur_move,
                     ev->time) != GrabSuccess)
        return;
    desk_busy = 1;
    memset(&last, 0, sizeof(last));

    for (;;) {
        XNextEvent(sc.dpy, &e);
        if (e.type == MotionNotify) {
            int nx, ny;

            while (XCheckMaskEvent(sc.dpy, PointerMotionMask, &last))
                e = last;
            nx = e.xmotion.x_root - dx0;
            ny = e.xmotion.y_root - dy0;
            if (nx < DESK_PAD)
                nx = DESK_PAD;
            if (nx > sc.sw - DESK_ICON_W - DESK_PAD)
                nx = sc.sw - DESK_ICON_W - DESK_PAD;
            if (ny < top_limit())
                ny = top_limit();
            if (ny > bottom_limit())
                ny = bottom_limit();
            if (nx == icons[idx].x && ny == icons[idx].y)
                continue;
            desk_clear(&icons[idx]);
            icons[idx].x = nx;
            icons[idx].y = ny;
            moved = 1;
            desktop_paint();
        } else if (e.type == ButtonRelease) {
            break;
        } else if (e.type == ButtonPress) {
            /* a second click arrived while we were waiting for the release:
               remember it and handle it after the drag so that a double
               click is never swallowed */
            pending = e;
            has_pending = 1;
            break;
        } else if (e.type == Expose) {
            if (e.xexpose.window == sc.root)
                desktop_expose(&e.xexpose);
            else
                handle_expose(&e.xexpose);
        } else if (e.type == MapRequest) {
            handle_map_request(&e.xmaprequest);
        } else if (e.type == ConfigureRequest) {
            handle_configure_request(&e.xconfigurerequest);
        } else if (e.type == DestroyNotify) {
            handle_destroy_notify(&e.xdestroywindow);
        } else if (e.type == UnmapNotify) {
            handle_unmap_notify(&e.xunmap);
        } else if (e.type == PropertyNotify) {
            handle_property_notify(&e.xproperty);
        } else if (e.type == ClientMessage) {
            handle_client_message(&e.xclient);
        } else if (e.type == EnterNotify) {
            handle_enter_notify(&e.xcrossing);
        }
    }
    XUngrabPointer(sc.dpy, CurrentTime);
    desk_busy = 0;

    if (moved) {
        icons[idx].custom = 1;
        desk_save_layout();
        log_msg("desktop: moved '%s' to %d,%d", icons[idx].name,
                icons[idx].x, icons[idx].y);
    }
    XFlush(sc.dpy);

    if (has_pending)
        handle_button_press(&pending.xbutton);   /* the click we swallowed */
}

int desktop_button(XButtonEvent *ev)
{
    int i;

    if (desk_busy || menu_is_open() || cmd_is_open() || fm_open() ||
        msgbox_open())
        return 0;
    if (ev->button == Button4 || ev->button == Button5)
        return 0;
    if (strip_mods(ev->state) & Mod1Mask)
        return 0;                       /* Alt+click belongs to the WM */

    if (ev->button == Button3) {
        i = desk_hit(ev->x_root, ev->y_root);
        if (i < 0)
            return 0;                   /* empty desktop: its own menu */
        desk_select(i);
        desk_icon_menu(ev, i);
        return 1;
    }
    if (ev->button != Button1)
        return 0;

    i = desk_hit(ev->x_root, ev->y_root);
    if (i < 0) {
        desk_select(-1);
        return 1;
    }
    if (i == sel && last_click_idx == i &&
        ev->time - last_click_time < DOUBLE_MS) {
        last_click_idx = -1;
        desktop_open(i);
        return 1;
    }
    desk_select(i);
    last_click_idx = i;
    last_click_time = ev->time;
    desk_drag(i, ev);
    return 1;
}

int desktop_motion(XMotionEvent *ev)
{
    int i;

    if (desk_busy || menu_is_open() || cmd_is_open() || fm_open())
        return 0;
    i = desk_hit(ev->x_root, ev->y_root);
    if (i == hover)
        return 1;
    if (hover >= 0 && hover < nicons)
        desk_clear(&icons[hover]);
    hover = i;
    desktop_paint();
    return 1;
}

int desktop_expose(XExposeEvent *ev)
{
    if (ev->count != 0)
        return 1;
    desktop_paint();
    return 1;
}

void desktop_root_property(XPropertyEvent *ev)
{
    if (ev->window != sc.root)
        return;
    if (!atom_root_pmap)
        atom_root_pmap = XInternAtom(sc.dpy, "_XROOTPMAP_ID", False);
    if (ev->atom != atom_root_pmap)
        return;
    desk_clear_all();                   /* a new wallpaper replaced ours */
    desktop_paint();
}

/* --------------------------------------------------------------- public -- */

void desktop_init(void)
{
    const char *home = getenv("HOME");
    const char *data = getenv("XDG_DATA_HOME");

    if (!home || !*home)
        home = "/tmp";
    snprintf(desk_dir, sizeof(desk_dir), "%s/Desktop", home);
    if (data && *data)
        snprintf(layout_path, sizeof(layout_path), "%s/scde/desktop.layout",
                 data);
    else
        snprintf(layout_path, sizeof(layout_path),
                 "%s/.local/share/scde/desktop.layout", home);

    desk_scan();
    desktop_paint();
}

void desktop_refresh(void)
{
    desk_clear_all();
    desk_scan();
    desktop_paint();
}

void desktop_cleanup(void)
{
    free(icons);
    icons = NULL;
    nicons = 0;
    sel = hover = -1;
}
