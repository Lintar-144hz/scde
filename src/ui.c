#include "scde.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define UI_ACCENT       0x0a84ffUL
#define UI_ACCENT_SOFT  0x2f6fedUL
#define UI_GREEN        0x28c840UL
#define UI_YELLOW       0xfebc2eUL
#define UI_RED          0xff5f57UL

typedef Bool (*UiShapeQueryFn)(Display *, int *, int *);
typedef void (*UiShapeMaskFn)(Display *, Window, int, int, int, Pixmap, int);

static UiShapeQueryFn shape_query;
static UiShapeMaskFn shape_mask;
static int shape_loaded;

static XFontStruct *load_first_font(const char *const *patterns)
{
    XFontStruct *f;
    int i;

    for (i = 0; patterns[i]; i++) {
        f = XLoadQueryFont(sc.dpy, patterns[i]);
        if (f)
            return f;
    }
    return NULL;
}

void ui_load_fonts(void)
{
    static const char *const regular[] = {
        "-adobe-helvetica-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "-urw-nimbus sans l-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-helvetica-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-fixed-medium-r-normal--13-*-*-*-*-*-*-*",
        "9x15", "fixed", NULL
    };
    static const char *const bold[] = {
        "-adobe-helvetica-bold-r-normal--13-*-*-*-*-*-iso8859-1",
        "-urw-nimbus sans l-bold-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-helvetica-bold-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-fixed-bold-r-normal--13-*-*-*-*-*-*-*",
        "9x15bold", "fixed", NULL
    };
    static const char *const small[] = {
        "-adobe-helvetica-medium-r-normal--11-*-*-*-*-*-iso8859-1",
        "-urw-nimbus sans l-medium-r-normal--11-*-*-*-*-*-iso8859-1",
        "-*-helvetica-medium-r-normal--11-*-*-*-*-*-iso8859-1",
        "-*-fixed-medium-r-normal--11-*-*-*-*-*-*-*",
        "7x13", "fixed", NULL
    };
    static const char *const mono[] = {
        "-urw-nimbus mono l-regular-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-courier-medium-r-normal--13-*-*-*-*-*-iso8859-1",
        "-*-fixed-medium-r-normal--13-*-*-*-*-*-*-*",
        "9x15", "fixed", NULL
    };

    if (sc.font)
        sc.font = NULL;
    if (cfg.font[0])
        sc.font = XLoadQueryFont(sc.dpy, cfg.font);
    if (!sc.font)
        sc.font = load_first_font(regular);
    sc.font_bold = load_first_font(bold);
    sc.font_small = load_first_font(small);
    sc.font_mono = load_first_font(mono);
    if (!sc.font_bold)
        sc.font_bold = sc.font;
    if (!sc.font_small)
        sc.font_small = sc.font;
    if (!sc.font_mono)
        sc.font_mono = sc.font;

    if (sc.font) {
        sc.font_ascent = sc.font->ascent;
        sc.font_descent = sc.font->descent;
        sc.font_height = sc.font_ascent + sc.font_descent;
    } else {
        sc.font_ascent = 10;
        sc.font_descent = 3;
        sc.font_height = 13;
    }
    sc.font_bold_ascent = sc.font_bold ? sc.font_bold->ascent : sc.font_ascent;
    sc.font_bold_descent = sc.font_bold ? sc.font_bold->descent
                                        : sc.font_descent;
    sc.font_small_ascent = sc.font_small ? sc.font_small->ascent
                                         : sc.font_ascent;
    sc.font_small_descent = sc.font_small ? sc.font_small->descent
                                          : sc.font_descent;
    sc.font_mono_ascent = sc.font_mono ? sc.font_mono->ascent
                                       : sc.font_ascent;
    sc.font_mono_descent = sc.font_mono ? sc.font_mono->descent
                                        : sc.font_descent;
}

void ui_cleanup_fonts(void)
{
    if (sc.font_bold && sc.font_bold != sc.font)
        XFreeFont(sc.dpy, sc.font_bold);
    if (sc.font_small && sc.font_small != sc.font &&
        sc.font_small != sc.font_bold)
        XFreeFont(sc.dpy, sc.font_small);
    if (sc.font_mono && sc.font_mono != sc.font &&
        sc.font_mono != sc.font_bold && sc.font_mono != sc.font_small)
        XFreeFont(sc.dpy, sc.font_mono);
    if (sc.font)
        XFreeFont(sc.dpy, sc.font);
    sc.font = sc.font_bold = sc.font_small = sc.font_mono = NULL;
}

int text_width_font(XFontStruct *f, const char *s)
{
    if (!s)
        return 0;
    if (f)
        return XTextWidth(f, s, (int)strlen(s));
    return (int)strlen(s) * 7;
}

int text_width_bold(const char *s)
{
    return text_width_font(sc.font_bold, s);
}

int text_width_small(const char *s)
{
    return text_width_font(sc.font_small, s);
}

int text_width_mono(const char *s)
{
    return text_width_font(sc.font_mono, s);
}

void draw_text_font(Window w, XFontStruct *f, int x, int baseline_y,
                    unsigned long color, const char *s)
{
    int len;

    if (!s)
        return;
    len = (int)strlen(s);
    if (len == 0)
        return;
    XSetForeground(sc.dpy, sc.gc, color);
    if (f)
        XSetFont(sc.dpy, sc.gc, f->fid);
    XDrawString(sc.dpy, w, sc.gc, x, baseline_y, s, len);
}

void draw_text_bold(Window w, int x, int baseline_y, unsigned long color,
                    const char *s)
{
    draw_text_font(w, sc.font_bold, x, baseline_y, color, s);
}

void draw_text_small(Window w, int x, int baseline_y, unsigned long color,
                     const char *s)
{
    draw_text_font(w, sc.font_small, x, baseline_y, color, s);
}

void draw_text_mono(Window w, int x, int baseline_y, unsigned long color,
                    const char *s)
{
    draw_text_font(w, sc.font_mono, x, baseline_y, color, s);
}

static void fit_text_font(char *dst, size_t dst_size, XFontStruct *f,
                          const char *src, int max_w)
{
    size_t len;

    if (dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    str_copy(dst, dst_size, src);
    if (text_width_font(f, dst) <= max_w)
        return;
    len = strlen(dst);
    while (len > 0) {
        len--;
        dst[len] = '\0';
        if (len >= 3) {
            dst[len - 3] = '.';
            dst[len - 2] = '.';
            dst[len - 1] = '.';
            dst[len] = '\0';
        }
        if (text_width_font(f, dst) <= max_w)
            return;
        if (len < 4)
            break;
    }
    dst[0] = '\0';
}

void draw_text_clip_font(Window w, XFontStruct *f, int x, int max_x,
                         int baseline_y, unsigned long color, const char *s)
{
    char buf[NAME_MAX_LEN];
    int avail = max_x - x;

    if (avail <= 4 || !s)
        return;
    fit_text_font(buf, sizeof(buf), f, s, avail);
    draw_text_font(w, f, x, baseline_y, color, buf);
}

unsigned long ui_mix(unsigned long a, unsigned long b, int pct)
{
    int r1 = (int)((a >> 16) & 0xff), g1 = (int)((a >> 8) & 0xff);
    int b1 = (int)(a & 0xff);
    int r2 = (int)((b >> 16) & 0xff), g2 = (int)((b >> 8) & 0xff);
    int b2 = (int)(b & 0xff);
    int r = (r1 * (100 - pct) + r2 * pct) / 100;
    int g = (g1 * (100 - pct) + g2 * pct) / 100;
    int bl = (b1 * (100 - pct) + b2 * pct) / 100;

    return (unsigned long)r << 16 | (unsigned long)g << 8 | (unsigned long)bl;
}

unsigned long ui_dim(unsigned long c, unsigned long bg)
{
    return ui_mix(c, bg, 58);
}

unsigned long ui_wallpaper_color(int y)
{
    int r1, g1, b1, r2, g2, b2, r, g, b;
    int denom = sc.sh > 1 ? sc.sh - 1 : 1;

    if (!color_rgb(cfg.s_wallpaper_top, &r1, &g1, &b1) ||
        !color_rgb(cfg.s_wallpaper_bottom, &r2, &g2, &b2))
        return cfg.col_background;
    if (y < 0)
        y = 0;
    if (y > sc.sh - 1)
        y = sc.sh - 1;
    r = r1 + ((r2 - r1) * y) / denom;
    g = g1 + ((g2 - g1) * y) / denom;
    b = b1 + ((b2 - b1) * y) / denom;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (unsigned long)r << 16 | (unsigned long)g << 8 | (unsigned long)b;
}

unsigned long ui_accent(void) { return UI_ACCENT; }
unsigned long ui_accent_soft(void) { return UI_ACCENT_SOFT; }
unsigned long ui_green(void) { return UI_GREEN; }
unsigned long ui_yellow(void) { return UI_YELLOW; }
unsigned long ui_red(void) { return UI_RED; }

static void round_draw(Display *dpy, Drawable d, GC gc, int x, int y,
                       int wd, int hg, int r, unsigned long color)
{
    if (wd <= 0 || hg <= 0)
        return;
    if (r < 1)
        r = 1;
    if (r * 2 > wd)
        r = wd / 2;
    if (r * 2 > hg)
        r = hg / 2;
    XSetForeground(dpy, gc, color);
    XFillRectangle(dpy, d, gc, x + r, y, (unsigned)(wd - 2 * r),
                   (unsigned)hg);
    XFillRectangle(dpy, d, gc, x, y + r, (unsigned)wd,
                   (unsigned)(hg - 2 * r));
    XFillArc(dpy, d, gc, x, y, (unsigned)(2 * r), (unsigned)(2 * r),
             90 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + wd - 2 * r, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 0, 90 * 64);
    XFillArc(dpy, d, gc, x, y + hg - 2 * r, (unsigned)(2 * r),
             (unsigned)(2 * r), 180 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + wd - 2 * r, y + hg - 2 * r, (unsigned)(2 * r),
             (unsigned)(2 * r), 270 * 64, 90 * 64);
}

void ui_round_fill(Window w, int x, int y, int wd, int hg, int r,
                   unsigned long color)
{
    round_draw(sc.dpy, w, sc.gc, x, y, wd, hg, r, color);
}

static int round_inset(int y, int hg, int r)
{
    int dy, rr, dx;

    if (y < 0 || y >= hg || r < 1)
        return 0;
    if (y >= r && y < hg - r)
        return 0;
    dy = y < r ? r - y : y - (hg - r - 1);
    rr = r * r - dy * dy;
    if (rr < 0)
        rr = 0;
    dx = 0;
    while ((dx + 1) * (dx + 1) <= rr)
        dx++;
    return r - dx;
}

void ui_round_fill_gradient(Window w, int x, int y, int wd, int hg, int r,
                            unsigned long top, unsigned long bottom)
{
    int row, denom = hg > 1 ? hg - 1 : 1;

    if (r < 1 || wd <= 2 || hg <= 2) {
        ui_round_fill(w, x, y, wd, hg, r, top);
        return;
    }
    for (row = 0; row < hg; row++) {
        int inset = round_inset(row, hg, r);
        unsigned long c = ui_mix(top, bottom, row * 100 / denom);

        XSetForeground(sc.dpy, sc.gc, c);
        XFillRectangle(sc.dpy, w, sc.gc, x + inset, y + row,
                       (unsigned)(wd - 2 * inset), 1);
    }
}

void ui_round_outline(Window w, int x, int y, int wd, int hg, int r,
                      unsigned long color)
{
    if (wd <= 2 || hg <= 2)
        return;
    if (r < 1)
        r = 1;
    if (r * 2 > wd)
        r = wd / 2;
    if (r * 2 > hg)
        r = hg / 2;
    XSetForeground(sc.dpy, sc.gc, color);
    XDrawArc(sc.dpy, w, sc.gc, x, y, (unsigned)(2 * r), (unsigned)(2 * r),
             90 * 64, 90 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x + wd - 2 * r - 1, y, (unsigned)(2 * r),
             (unsigned)(2 * r), 0, 90 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x, y + hg - 2 * r - 1, (unsigned)(2 * r),
             (unsigned)(2 * r), 180 * 64, 90 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x + wd - 2 * r - 1, y + hg - 2 * r - 1,
             (unsigned)(2 * r), (unsigned)(2 * r), 270 * 64, 90 * 64);
    XDrawLine(sc.dpy, w, sc.gc, x + r, y, x + wd - r - 1, y);
    XDrawLine(sc.dpy, w, sc.gc, x + r, y + hg - 1, x + wd - r - 1,
              y + hg - 1);
    XDrawLine(sc.dpy, w, sc.gc, x, y + r, x, y + hg - r - 1);
    XDrawLine(sc.dpy, w, sc.gc, x + wd - 1, y + r, x + wd - 1,
              y + hg - r - 1);
}

void ui_shape_round(Window w, int wd, int hg, int r)
{
    Pixmap mask;
    GC mgc;
    int ev, er;

    if (!sc.dpy || w == None || wd <= 2 || hg <= 2)
        return;
    if (!shape_loaded) {
        void *h;

        shape_loaded = 1;
        h = dlopen("libXext.so.6", RTLD_LAZY | RTLD_LOCAL);
        if (!h)
            h = dlopen("libXext.so", RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            union { void *p; UiShapeQueryFn f; } uq;
            union { void *p; UiShapeMaskFn f; } um;

            uq.p = dlsym(h, "XShapeQueryExtension");
            um.p = dlsym(h, "XShapeCombineMask");
            shape_query = uq.f;
            shape_mask = um.f;
        }
        if (!shape_query || !shape_mask ||
            !shape_query(sc.dpy, &ev, &er)) {
            shape_query = NULL;
            shape_mask = NULL;
            return;
        }
    }
    if (!shape_mask)
        return;
    mask = XCreatePixmap(sc.dpy, sc.root, (unsigned)wd, (unsigned)hg, 1);
    if (mask == None)
        return;
    mgc = XCreateGC(sc.dpy, mask, 0, NULL);
    XSetForeground(sc.dpy, mgc, 0);
    XFillRectangle(sc.dpy, mask, mgc, 0, 0, (unsigned)wd, (unsigned)hg);
    round_draw(sc.dpy, mask, mgc, 0, 0, wd, hg, r, 1);
    shape_mask(sc.dpy, w, 0, 0, 0, mask, 0);
    XFreeGC(sc.dpy, mgc);
    XFreePixmap(sc.dpy, mask);
}

void ui_draw_traffic(Window w, int x, int cy, int active, int hover,
                     int maximized, unsigned long bg)
{
    static const unsigned long on[3] = { UI_RED, UI_YELLOW, UI_GREEN };
    int d = 13, gap = 8, i;

    for (i = 0; i < 3; i++) {
        int ox = x + i * (d + gap);
        int oy = cy - d / 2;
        unsigned long c = active ? on[i] : ui_mix(on[i], bg, 74);
        int dark = active ? 0x3a0000UL : ui_mix(bg, 0x000000UL, 35);

        XSetForeground(sc.dpy, sc.gc, c);
        XFillArc(sc.dpy, w, sc.gc, ox, oy, (unsigned)d, (unsigned)d, 0,
                 360 * 64);
        XSetForeground(sc.dpy, sc.gc,
                       ui_mix(c, 0xffffffUL, active ? 22 : 8));
        XDrawArc(sc.dpy, w, sc.gc, ox + 1, oy + 1, (unsigned)(d - 3),
                 (unsigned)(d - 3), 45 * 64, 90 * 64);
        if (hover != i)
            continue;
        if (i == UI_LIGHT_CLOSE) {
            XSetForeground(sc.dpy, sc.gc, dark);
            XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 4, ox + 8, oy + 8);
            XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 8, ox + 8, oy + 4);
        } else if (i == UI_LIGHT_MIN) {
            XSetForeground(sc.dpy, sc.gc, dark);
            XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 6, ox + 8, oy + 6);
            XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 7, ox + 8, oy + 7);
        } else {
            XSetForeground(sc.dpy, sc.gc, dark);
            if (maximized) {
                XDrawRectangle(sc.dpy, w, sc.gc, ox + 4, oy + 3, 4, 4);
                XDrawRectangle(sc.dpy, w, sc.gc, ox + 6, oy + 5, 4, 4);
            } else {
                XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 8, ox + 8, oy + 4);
                XDrawLine(sc.dpy, w, sc.gc, ox + 7, oy + 4, ox + 8, oy + 4);
                XDrawLine(sc.dpy, w, sc.gc, ox + 8, oy + 4, ox + 8, oy + 5);
                XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 8, ox + 4, oy + 7);
                XDrawLine(sc.dpy, w, sc.gc, ox + 4, oy + 7, ox + 5, oy + 8);
            }
        }
    }
}

void ui_draw_folder(Window w, int x, int y, int size, int open)
{
    int r = size / 8;
    int tab_w = size * 52 / 100;
    int body_y = y + size / 5;
    int body_h = size - (body_y - y);
    unsigned long back = open ? 0x2a6fc9UL : 0x3f8be8UL;
    unsigned long top = open ? 0x74b8ffUL : 0x68b5ffUL;
    unsigned long bottom = open ? 0x2f7cd8UL : 0x2f7ed8UL;

    if (size < 8)
        return;
    ui_round_fill(w, x + size / 12, y + size / 20,
                  tab_w, (size / 4), r, back);
    ui_round_fill_gradient(w, x, body_y, size, body_h, r, top, bottom);
    XSetForeground(sc.dpy, sc.gc, ui_mix(top, 0xffffffUL, 26));
    XFillRectangle(sc.dpy, w, sc.gc, x + r, body_y + 1,
                   (unsigned)(size - 2 * r), 1);
    if (open) {
        XSetForeground(sc.dpy, sc.gc, 0x8ecaffUL);
        XFillRectangle(sc.dpy, w, sc.gc, x + size / 8, body_y + size / 8,
                       (unsigned)(size * 3 / 4), (unsigned)(size / 12));
    }
}

void ui_draw_file(Window w, int x, int y, int size, unsigned long accent)
{
    int fold = size / 4;
    int r = size / 14;

    if (size < 8)
        return;
    ui_round_fill(w, x, y + 1, size, size - 2, r,
                  ui_mix(0xffffffUL, 0xd9e2eeUL, 22));
    XSetForeground(sc.dpy, sc.gc, ui_mix(0xffffffUL, 0xbcc8d8UL, 40));
    XFillRectangle(sc.dpy, w, sc.gc, x + size - fold, y + 1,
                   (unsigned)fold, (unsigned)fold);
    XSetForeground(sc.dpy, sc.gc, 0xf3f6faUL);
    XFillRectangle(sc.dpy, w, sc.gc, x + size - fold + 1, y + 1,
                   (unsigned)(fold - 1), (unsigned)(fold - 1));
    XSetForeground(sc.dpy, sc.gc, accent);
    XFillRectangle(sc.dpy, w, sc.gc, x + size / 6,
                   y + size * 3 / 5, (unsigned)(size * 2 / 3),
                   (unsigned)(size / 10));
    XSetForeground(sc.dpy, sc.gc, ui_mix(accent, 0xffffffUL, 35));
    XFillRectangle(sc.dpy, w, sc.gc, x + size / 6,
                   y + size / 2, (unsigned)(size / 2),
                   (unsigned)(size / 16));
}

void ui_draw_launcher_mark(Window w, int cx, int cy, int size,
                           unsigned long fg)
{
    int gap = size / 8;
    int tile = (size - gap) / 2;
    int x0 = cx - size / 2;
    int y0 = cy - size / 2;
    int i, j;

    (void)fg;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            unsigned long c = (i == j) ? UI_ACCENT : 0xffffffUL;

            ui_round_fill(w, x0 + i * (tile + gap), y0 + j * (tile + gap),
                          tile, tile, tile / 4, c);
        }
    }
}

static unsigned long icon_hue(unsigned int h)
{
    static const unsigned long pal[] = {
        0x2f7cf6UL, 0x5e5ce6UL, 0xbf5af2UL, 0xff375fUL, 0xff9f0aUL,
        0x30d158UL, 0x64d2ffUL, 0x8e8e93UL, 0x0a84ffUL, 0xd64f8cUL
    };

    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

static void icon_generic(Window w, int x, int y, int size, int dim,
                         const char *initial, unsigned long base)
{
    unsigned long top = dim ? ui_mix(base, 0x1f2026UL, 62)
                            : ui_mix(base, 0xffffffUL, 24);
    unsigned long bottom = dim ? ui_mix(base, 0x1f2026UL, 78)
                               : ui_mix(base, 0x000000UL, 18);
    int fs = size * 45 / 100;
    char letter[2];

    ui_round_fill_gradient(w, x, y, size, size, size / 4, top, bottom);
    ui_round_outline(w, x, y, size, size, size / 4,
                     dim ? 0x34343aUL : ui_mix(base, 0xffffffUL, 32));
    if (!initial || !initial[0])
        initial = "?";
    letter[0] = initial[0];
    letter[1] = '\0';
    (void)fs;
    draw_text_font(w, sc.font_bold, x + (size - text_width_bold(letter)) / 2,
                   y + (size + sc.font_bold_ascent -
                        sc.font_bold_descent) / 2,
                   0xffffffUL, letter);
}

void ui_draw_app_icon(Window w, int kind, int x, int y, int size, int dim,
                      const char *initial)
{
    unsigned int h = 0;
    const char *p;

    if (size < 8)
        return;
    if (kind == UI_ICON_GENERIC) {
        for (p = initial ? initial : ""; *p; p++)
            h = h * 31u + (unsigned int)(unsigned char)*p;
        icon_generic(w, x, y, size, dim, initial, icon_hue(h));
        return;
    }
    if (kind == UI_ICON_LAUNCHER) {
        icon_generic(w, x, y, size, dim, NULL, 0x3a3a42UL);
        ui_draw_launcher_mark(w, x + size / 2, y + size / 2,
                              size * 62 / 100, 0xffffffUL);
        return;
    }
    if (kind == UI_ICON_FILES) {
        unsigned long c = dim ? 0x5d6470UL : 0xf5f7fbUL;
        int half = size / 2;
        int inset = size / 10;

        ui_round_fill(w, x, y, size, size, size / 4, 0x2f7cf6UL);
        XSetForeground(sc.dpy, sc.gc, c);
        XFillRectangle(sc.dpy, w, sc.gc, x + inset, y + inset,
                       (unsigned)half, (unsigned)(size - 2 * inset));
        XSetForeground(sc.dpy, sc.gc, 0xffffffUL);
        XDrawArc(sc.dpy, w, sc.gc, x + size / 8, y + size / 3,
                 (unsigned)(size * 3 / 4), (unsigned)(size / 3),
                 200 * 64, 140 * 64);
        XFillRectangle(sc.dpy, w, sc.gc, x + size * 28 / 100,
                       y + size * 34 / 100, 2, (unsigned)(size / 12));
        XFillRectangle(sc.dpy, w, sc.gc, x + size * 60 / 100,
                       y + size * 34 / 100, 2, (unsigned)(size / 12));
        return;
    }
    if (kind == UI_ICON_TERMINAL) {
        unsigned long base = dim ? 0x2a2a2fUL : 0x2c2c30UL;

        ui_round_fill_gradient(w, x, y, size, size, size / 4,
                               ui_mix(base, 0xffffffUL, 10), base);
        XSetForeground(sc.dpy, sc.gc, 0x3c3c42UL);
        XFillRectangle(sc.dpy, w, sc.gc, x + size / 5, y + size / 4,
                       (unsigned)(size * 3 / 5), (unsigned)(size / 18));
        XSetForeground(sc.dpy, sc.gc, 0x30d158UL);
        draw_text_mono(w, x + size / 5, y + size * 3 / 5,
                       dim ? 0x7a7a80UL : 0x30d158UL, ">_");
        return;
    }
    if (kind == UI_ICON_BROWSER) {
        int cx = x + size / 2;
        int cy = y + size / 2;
        int rr = size * 36 / 100;
        XPoint needle[3];

        ui_round_fill_gradient(w, x, y, size, size, size / 4,
                               dim ? 0x31506eUL : 0x54b8ffUL,
                               dim ? 0x24384fUL : 0x0a6fd8UL);
        XSetForeground(sc.dpy, sc.gc,
                       dim ? 0x9aa0aaUL : 0xf6f9ffUL);
        XFillArc(sc.dpy, w, sc.gc, cx - rr, cy - rr, (unsigned)(2 * rr),
                 (unsigned)(2 * rr), 0, 360 * 64);
        needle[0].x = (short)cx;
        needle[0].y = (short)(cy - rr + 3);
        needle[1].x = (short)(cx + rr / 3);
        needle[1].y = (short)(cy + rr / 4);
        needle[2].x = (short)(cx - rr / 4);
        needle[2].y = (short)(cy + rr / 3);
        XSetForeground(sc.dpy, sc.gc, dim ? 0x6b7078UL : 0xff3b30UL);
        XFillPolygon(sc.dpy, w, sc.gc, needle, 3, Convex,
                     CoordModeOrigin);
        XSetForeground(sc.dpy, sc.gc, dim ? 0x6b7078UL : 0x0a84ffUL);
        XDrawLine(sc.dpy, w, sc.gc, cx, cy - rr + 3, cx - rr / 4,
                  cy + rr / 3);
        return;
    }
    if (kind == UI_ICON_SECURITY) {
        ui_round_fill_gradient(w, x, y, size, size, size / 4,
                               dim ? 0x2e4260UL : 0x3f8dffUL,
                               dim ? 0x1e2e46UL : 0x0a57c9UL);
        ui_draw_shield(w, x + size * 26 / 100, y + size / 2,
                       size * 48 / 100, dim ? 0x8b94a3UL : 0xffffffUL);
        return;
    }
    if (kind == UI_ICON_SETTINGS) {
        unsigned long base = dim ? 0x4c4c52UL : 0x8e8e93UL;

        ui_round_fill_gradient(w, x, y, size, size, size / 4,
                               ui_mix(base, 0xffffffUL, 30),
                               ui_mix(base, 0x000000UL, 22));
        ui_draw_gear(w, x + size / 2, y + size / 2, size * 54 / 100,
                     dim ? 0x77777dUL : 0xf7f7faUL);
        return;
    }
    icon_generic(w, x, y, size, dim, initial, 0x3a3a42UL);
}

void ui_draw_chevron(Window w, int cx, int cy, int dir, unsigned long color,
                     int size)
{
    XSetForeground(sc.dpy, sc.gc, color);
    if (dir < 0) {
        XDrawLine(sc.dpy, w, sc.gc, cx + size / 3, cy - size / 2,
                  cx - size / 3, cy);
        XDrawLine(sc.dpy, w, sc.gc, cx - size / 3, cy,
                  cx + size / 3, cy + size / 2);
    } else if (dir > 0) {
        XDrawLine(sc.dpy, w, sc.gc, cx - size / 3, cy - size / 2,
                  cx + size / 3, cy);
        XDrawLine(sc.dpy, w, sc.gc, cx + size / 3, cy,
                  cx - size / 3, cy + size / 2);
    } else {
        XDrawLine(sc.dpy, w, sc.gc, cx - size / 2, cy + size / 3,
                  cx, cy - size / 3);
        XDrawLine(sc.dpy, w, sc.gc, cx, cy - size / 3,
                  cx + size / 2, cy + size / 3);
    }
}

void ui_draw_magnifier(Window w, int x, int y, int size, unsigned long color)
{
    int d = size * 3 / 5;

    XSetForeground(sc.dpy, sc.gc, color);
    XDrawArc(sc.dpy, w, sc.gc, x, y, (unsigned)d, (unsigned)d, 0,
             360 * 64);
    XDrawLine(sc.dpy, w, sc.gc, x + d - 1, y + d - 1,
              x + size - 1, y + size - 1);
    XDrawLine(sc.dpy, w, sc.gc, x + d - 2, y + d - 1,
              x + size - 1, y + size - 2);
}

void ui_draw_toggle(Window w, int x, int y, int wd, int hg, int on,
                    int hover)
{
    int knob = hg - 4;
    int kx = on ? x + wd - knob - 2 : x + 2;
    unsigned long bg = on ? (hover ? 0x3f9cffUL : UI_ACCENT)
                          : (hover ? 0x5a5a61UL : 0x48484fUL);

    (void)hover;
    ui_round_fill(w, x, y, wd, hg, hg / 2, bg);
    XSetForeground(sc.dpy, sc.gc, ui_mix(0xffffffUL, 0x000000UL, 12));
    XFillArc(sc.dpy, w, sc.gc, kx, y + 2, (unsigned)knob, (unsigned)knob,
             0, 360 * 64);
    XSetForeground(sc.dpy, sc.gc, 0xffffffUL);
    XFillArc(sc.dpy, w, sc.gc, kx + 1, y + 3, (unsigned)(knob - 2),
             (unsigned)(knob - 2), 0, 360 * 64);
}

void ui_draw_wifi(Window w, int x, int cy, unsigned long fg)
{
    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawArc(sc.dpy, w, sc.gc, x + 1, cy - 6, 14, 12, 35 * 64, 110 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x + 3, cy - 3, 10, 9, 35 * 64, 110 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x + 5, cy, 6, 6, 35 * 64, 110 * 64);
    XFillArc(sc.dpy, w, sc.gc, x + 6, cy + 4, 4, 4, 0, 360 * 64);
}

void ui_draw_volume(Window w, int x, int cy, unsigned long fg)
{
    XPoint sp[5];

    sp[0].x = (short)(x + 2);
    sp[0].y = (short)(cy - 3);
    sp[1].x = (short)(x + 5);
    sp[1].y = (short)(cy - 3);
    sp[2].x = (short)(x + 9);
    sp[2].y = (short)(cy - 7);
    sp[3].x = (short)(x + 9);
    sp[3].y = (short)(cy + 7);
    sp[4].x = (short)(x + 5);
    sp[4].y = (short)(cy + 3);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, w, sc.gc, sp, 5, Convex, CoordModeOrigin);
    XDrawArc(sc.dpy, w, sc.gc, x + 7, cy - 6, 8, 12, 300 * 64, 120 * 64);
    XDrawArc(sc.dpy, w, sc.gc, x + 9, cy - 9, 12, 18, 300 * 64, 120 * 64);
}

void ui_draw_battery(Window w, int x, int cy, unsigned long fg,
                     unsigned long bg)
{
    (void)bg;
    XSetForeground(sc.dpy, sc.gc, fg);
    ui_round_outline(w, x, cy - 5, 20, 11, 3, fg);
    XFillRectangle(sc.dpy, w, sc.gc, x + 21, cy - 2, 2, 5);
    XSetForeground(sc.dpy, sc.gc, fg);
    ui_round_fill(w, x + 2, cy - 3, 15, 7, 1, fg);
}

void ui_draw_shield(Window w, int x, int cy, int size, unsigned long fg)
{
    XPoint p[6];
    int h = size / 2;

    p[0].x = (short)(x + h);
    p[0].y = (short)(cy - size / 2);
    p[1].x = (short)(x + size);
    p[1].y = (short)(cy - size / 3);
    p[2].x = (short)(x + size * 4 / 5);
    p[2].y = (short)(cy + size / 3);
    p[3].x = (short)(x + h);
    p[3].y = (short)(cy + size / 2);
    p[4].x = (short)(x + size / 5);
    p[4].y = (short)(cy + size / 3);
    p[5].x = (short)x;
    p[5].y = (short)(cy - size / 3);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, w, sc.gc, p, 6, Convex, CoordModeOrigin);
}

void ui_draw_gear(Window w, int cx, int cy, int size, unsigned long fg)
{
    int r = size / 2;
    int i;

    XSetForeground(sc.dpy, sc.gc, fg);
    for (i = 0; i < 8; i++) {
        int a = i * 45;
        int x = cx + (r * 8 / 10);
        int y = cy;

        if (a == 0)
            x = cx + r;
        else if (a == 45)
            x = cx + r * 7 / 10, y = cy - r * 7 / 10;
        else if (a == 90)
            x = cx, y = cy - r;
        else if (a == 135)
            x = cx - r * 7 / 10, y = cy - r * 7 / 10;
        else if (a == 180)
            x = cx - r;
        else if (a == 225)
            x = cx - r * 7 / 10, y = cy + r * 7 / 10;
        else if (a == 270)
            x = cx, y = cy + r;
        else
            x = cx + r * 7 / 10, y = cy + r * 7 / 10;
        XFillRectangle(sc.dpy, w, sc.gc, x - 2, y - 2, 4, 4);
    }
    XFillArc(sc.dpy, w, sc.gc, cx - r * 7 / 10, cy - r * 7 / 10,
             (unsigned)(r * 14 / 10), (unsigned)(r * 14 / 10), 0, 360 * 64);
    XSetForeground(sc.dpy, sc.gc, cfg.col_panel_bg);
    XFillArc(sc.dpy, w, sc.gc, cx - r * 3 / 10, cy - r * 3 / 10,
             (unsigned)(r * 6 / 10), (unsigned)(r * 6 / 10), 0, 360 * 64);
}

void ui_draw_house(Window w, int x, int cy, int size, unsigned long fg)
{
    XPoint roof[3];

    roof[0].x = (short)(x + size / 2);
    roof[0].y = (short)(cy - size / 2);
    roof[1].x = (short)(x + size);
    roof[1].y = (short)(cy);
    roof[2].x = (short)x;
    roof[2].y = (short)cy;
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, w, sc.gc, roof, 3, Convex, CoordModeOrigin);
    XFillRectangle(sc.dpy, w, sc.gc, x + size / 4, cy,
                   (unsigned)(size / 2), (unsigned)(size / 2));
}

void ui_draw_star(Window w, int cx, int cy, int size, unsigned long fg)
{
    XPoint p[10];
    int ro = size / 2;
    int ri = ro * 4 / 10;

    p[0].x = (short)cx;
    p[0].y = (short)(cy - ro);
    p[1].x = (short)(cx + ri / 3);
    p[1].y = (short)(cy - ri / 3);
    p[2].x = (short)(cx + ro);
    p[2].y = (short)(cy - ri / 4);
    p[3].x = (short)(cx + ri / 2);
    p[3].y = (short)(cy + ri / 4);
    p[4].x = (short)(cx + ro * 3 / 4);
    p[4].y = (short)(cy + ro);
    p[5].x = (short)cx;
    p[5].y = (short)(cy + ri / 2);
    p[6].x = (short)(cx - ro * 3 / 4);
    p[6].y = (short)(cy + ro);
    p[7].x = (short)(cx - ri / 2);
    p[7].y = (short)(cy + ri / 4);
    p[8].x = (short)(cx - ro);
    p[8].y = (short)(cy - ri / 4);
    p[9].x = (short)(cx - ri / 3);
    p[9].y = (short)(cy - ri / 3);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillPolygon(sc.dpy, w, sc.gc, p, 10, Complex, CoordModeOrigin);
}

void ui_draw_clock_icon(Window w, int cx, int cy, int size, unsigned long fg)
{
    int r = size / 2;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawArc(sc.dpy, w, sc.gc, cx - r, cy - r, (unsigned)(2 * r),
             (unsigned)(2 * r), 0, 360 * 64);
    XDrawLine(sc.dpy, w, sc.gc, cx, cy - r * 2 / 3, cx, cy);
    XDrawLine(sc.dpy, w, sc.gc, cx, cy, cx + r / 2, cy + r / 3);
}

void ui_draw_doc_icon(Window w, int x, int cy, int size, unsigned long fg)
{
    int fold = size / 3;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawRectangle(sc.dpy, w, sc.gc, x, cy - size / 2, (unsigned)(size - 1),
                   (unsigned)(size - 1));
    XDrawLine(sc.dpy, w, sc.gc, x + size - fold, cy - size / 2,
              x + size - fold, cy - size / 2 + fold);
    XDrawLine(sc.dpy, w, sc.gc, x + size - fold, cy - size / 2 + fold,
              x + size - 1, cy - size / 2 + fold);
    XDrawLine(sc.dpy, w, sc.gc, x + size / 5, cy - size / 5,
              x + size * 4 / 5, cy - size / 5);
    XDrawLine(sc.dpy, w, sc.gc, x + size / 5, cy,
              x + size * 4 / 5, cy);
}

void ui_draw_download_icon(Window w, int cx, int cy, int size,
                           unsigned long fg)
{
    int r = size / 2;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawLine(sc.dpy, w, sc.gc, cx, cy - r, cx, cy + r / 3);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 3, cy, cx, cy + r / 3);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 3, cy, cx, cy + r / 3);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 2, cy + r / 2,
              cx + r / 2, cy + r / 2);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 2, cy + r / 2,
              cx - r / 2, cy + r / 4);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 2, cy + r / 2,
              cx + r / 2, cy + r / 4);
}

void ui_draw_music_icon(Window w, int cx, int cy, int size, unsigned long fg)
{
    int r = size / 2;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 3, cy - r,
              cx + r / 3, cy + r / 3);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 3, cy - r,
              cx - r / 3, cy - r * 3 / 4);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 3, cy - r * 3 / 4,
              cx - r / 3, cy + r / 2);
    XFillArc(sc.dpy, w, sc.gc, cx - r * 3 / 4, cy + r / 3,
             (unsigned)(r), (unsigned)(r), 0, 360 * 64);
    XFillArc(sc.dpy, w, sc.gc, cx + r / 6, cy + r / 6,
             (unsigned)(r), (unsigned)(r), 0, 360 * 64);
}

void ui_draw_image_icon(Window w, int x, int cy, int size, unsigned long fg)
{
    XPoint m[3];

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawRectangle(sc.dpy, w, sc.gc, x, cy - size / 2, (unsigned)(size - 1),
                   (unsigned)(size - 1));
    XFillArc(sc.dpy, w, sc.gc, x + size * 2 / 3, cy - size / 3,
             (unsigned)(size / 5), (unsigned)(size / 5), 0, 360 * 64);
    m[0].x = (short)(x + 2);
    m[0].y = (short)(cy + size / 2 - 2);
    m[1].x = (short)(x + size / 2);
    m[1].y = (short)(cy - size / 6);
    m[2].x = (short)(x + size - 2);
    m[2].y = (short)(cy + size / 2 - 2);
    XFillPolygon(sc.dpy, w, sc.gc, m, 3, Convex, CoordModeOrigin);
}

void ui_draw_film_icon(Window w, int x, int cy, int size, unsigned long fg)
{
    int i;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawRectangle(sc.dpy, w, sc.gc, x, cy - size / 2, (unsigned)(size - 1),
                   (unsigned)(size - 1));
    for (i = 0; i < 3; i++) {
        XFillRectangle(sc.dpy, w, sc.gc, x + 2, cy - size / 2 + 2 + i * size / 4,
                       2, 2);
        XFillRectangle(sc.dpy, w, sc.gc, x + size - 4,
                       cy - size / 2 + 2 + i * size / 4, 2, 2);
    }
}

void ui_draw_trash_icon(Window w, int cx, int cy, int size, unsigned long fg)
{
    int r = size / 2;

    XSetForeground(sc.dpy, sc.gc, fg);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 2, cy - r / 2,
              cx + r / 2, cy - r / 2);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 3, cy - r / 2,
              cx - r / 4, cy + r / 2);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 3, cy - r / 2,
              cx + r / 4, cy + r / 2);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 4, cy + r / 2,
              cx + r / 4, cy + r / 2);
    XDrawLine(sc.dpy, w, sc.gc, cx - r / 4, cy - r / 4,
              cx - r / 6, cy + r / 3);
    XDrawLine(sc.dpy, w, sc.gc, cx + r / 4, cy - r / 4,
              cx + r / 6, cy + r / 3);
}

void ui_draw_drive_icon(Window w, int x, int cy, int size, unsigned long fg)
{
    XSetForeground(sc.dpy, sc.gc, fg);
    ui_round_outline(w, x, cy - size / 3, size, size * 2 / 3, 3, fg);
    XDrawLine(sc.dpy, w, sc.gc, x + 2, cy + size / 12,
              x + size - 3, cy + size / 12);
    XFillRectangle(sc.dpy, w, sc.gc, x + size - size / 4,
                   cy + size / 5, 2, 2);
}
