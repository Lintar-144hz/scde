/*
 * SCDE - configuration  (~/.config/scde/config)
 *
 * Format:  key=value     lines starting with '#' are comments
 */
#include "scde.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));

    c->panel_top = 0;                    /* bottom panel (Plasma style) */
    c->panel_height = 32;
    c->border_width = 1;
    c->title_height = 24;
    c->focus_follows_mouse = 1;
    c->show_clock = 1;
    c->show_tray = 1;
    c->show_desktop_btn = 1;
    str_copy(c->clock_format, sizeof(c->clock_format), "%H:%M");
    str_copy(c->clock_date_format, sizeof(c->clock_date_format), "%b %d");
    str_copy(c->terminal, sizeof(c->terminal), "xterm");
    str_copy(c->browser, sizeof(c->browser), "");
    str_copy(c->font, sizeof(c->font),
             "-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*");

    /* Plasma-ish dark palette */
    str_copy(c->s_background, sizeof(c->s_background), "#0e1117");
    str_copy(c->s_wallpaper_top, sizeof(c->s_wallpaper_top), "#1b2436");
    str_copy(c->s_wallpaper_bottom, sizeof(c->s_wallpaper_bottom), "#0b0d12");
    str_copy(c->s_border_focus, sizeof(c->s_border_focus), "#3daee9");
    str_copy(c->s_border_unfocus, sizeof(c->s_border_unfocus), "#23262c");
    str_copy(c->s_title_bg, sizeof(c->s_title_bg), "#2b303a");
    str_copy(c->s_title_bg_inactive, sizeof(c->s_title_bg_inactive),
             "#22252c");
    str_copy(c->s_title_fg, sizeof(c->s_title_fg), "#eaeef3");
    str_copy(c->s_title_fg_inactive, sizeof(c->s_title_fg_inactive),
             "#8b93a1");
    str_copy(c->s_title_hover, sizeof(c->s_title_hover), "#39414d");
    str_copy(c->s_close_hover, sizeof(c->s_close_hover), "#d32f2f");
    str_copy(c->s_panel_bg, sizeof(c->s_panel_bg), "#171a20");
    str_copy(c->s_panel_fg, sizeof(c->s_panel_fg), "#c9d1db");
    str_copy(c->s_btn_active, sizeof(c->s_btn_active), "#2f5f8a");
    str_copy(c->s_btn_inactive, sizeof(c->s_btn_inactive), "#24272e");
    str_copy(c->s_btn_hover, sizeof(c->s_btn_hover), "#3a414c");
    str_copy(c->s_menu_bg, sizeof(c->s_menu_bg), "#1c1f26");
    str_copy(c->s_menu_fg, sizeof(c->s_menu_fg), "#d5dae2");
    str_copy(c->s_menu_sel, sizeof(c->s_menu_sel), "#2d5f8f");
}

static int to_int(const char *v, int lo, int hi, int fallback)
{
    char *end;
    long n;

    n = strtol(v, &end, 10);
    if (end == v || n < lo || n > hi)
        return fallback;
    return (int)n;
}

static void set_color_str(char *dst, size_t size, const char *v)
{
    if (v[0] == '#')
        str_copy(dst, size, v);
}

static void add_menu_entry(Config *c, const char *v)
{
    const char *sep;
    MenuEntry *e;

    if (c->menu_count >= MAX_MENU_ITEMS)
        return;
    sep = strchr(v, '|');
    if (!sep || sep == v) {
        log_msg("config: ignoring bad menu line '%s'", v);
        return;
    }
    e = &c->menu[c->menu_count];
    str_copy(e->label, sizeof(e->label), v);
    /* cut label at separator */
    {
        size_t len = (size_t)(sep - v);
        if (len >= sizeof(e->label))
            len = sizeof(e->label) - 1;
        memcpy(e->label, v, len);
        e->label[len] = '\0';
    }
    str_copy(e->cmd, sizeof(e->cmd), sep + 1);
    c->menu_count++;
}

int config_load(Config *c, const char *path)
{
    FILE *f;
    char line[600];
    int lineno = 0;

    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *s, *eq, *key, *val;

        lineno++;
        s = str_trim(line);
        if (*s == '\0' || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq) {
            log_msg("config %s:%d: missing '='", path, lineno);
            continue;
        }
        *eq = '\0';
        key = str_trim(s);
        val = str_trim(eq + 1);

        if (strcmp(key, "panel") == 0)
            c->panel_top = (strcmp(val, "top") == 0);
        else if (strcmp(key, "panel_height") == 0)
            c->panel_height = to_int(val, 16, 64, c->panel_height);
        else if (strcmp(key, "border_width") == 0)
            c->border_width = to_int(val, 0, 8, c->border_width);
        else if (strcmp(key, "focus_follows_mouse") == 0)
            c->focus_follows_mouse = to_int(val, 0, 1, 1);
        else if (strcmp(key, "show_clock") == 0)
            c->show_clock = to_int(val, 0, 1, 1);
        else if (strcmp(key, "title_height") == 0)
            c->title_height = to_int(val, 14, 48, c->title_height);
        else if (strcmp(key, "show_tray") == 0)
            c->show_tray = to_int(val, 0, 1, 1);
        else if (strcmp(key, "show_desktop_btn") == 0)
            c->show_desktop_btn = to_int(val, 0, 1, 1);
        else if (strcmp(key, "clock_format") == 0)
            str_copy(c->clock_format, sizeof(c->clock_format), val);
        else if (strcmp(key, "clock_date_format") == 0)
            str_copy(c->clock_date_format, sizeof(c->clock_date_format), val);
        else if (strcmp(key, "browser") == 0)
            str_copy(c->browser, sizeof(c->browser), val);
        else if (strcmp(key, "terminal") == 0)
            str_copy(c->terminal, sizeof(c->terminal), val);
        else if (strcmp(key, "font") == 0)
            str_copy(c->font, sizeof(c->font), val);
        else if (strcmp(key, "wallpaper") == 0)
            str_copy(c->wallpaper, sizeof(c->wallpaper), val);
        else if (strcmp(key, "menu") == 0)
            add_menu_entry(c, val);
        else if (strcmp(key, "background") == 0)
            set_color_str(c->s_background, sizeof(c->s_background), val);
        else if (strcmp(key, "border_focused") == 0)
            set_color_str(c->s_border_focus, sizeof(c->s_border_focus), val);
        else if (strcmp(key, "border_unfocused") == 0)
            set_color_str(c->s_border_unfocus, sizeof(c->s_border_unfocus), val);
        else if (strcmp(key, "panel_bg") == 0)
            set_color_str(c->s_panel_bg, sizeof(c->s_panel_bg), val);
        else if (strcmp(key, "panel_fg") == 0)
            set_color_str(c->s_panel_fg, sizeof(c->s_panel_fg), val);
        else if (strcmp(key, "button_active") == 0)
            set_color_str(c->s_btn_active, sizeof(c->s_btn_active), val);
        else if (strcmp(key, "button_inactive") == 0)
            set_color_str(c->s_btn_inactive, sizeof(c->s_btn_inactive), val);
        else if (strcmp(key, "menu_bg") == 0)
            set_color_str(c->s_menu_bg, sizeof(c->s_menu_bg), val);
        else if (strcmp(key, "menu_fg") == 0)
            set_color_str(c->s_menu_fg, sizeof(c->s_menu_fg), val);
        else if (strcmp(key, "menu_selected") == 0)
            set_color_str(c->s_menu_sel, sizeof(c->s_menu_sel), val);
        else if (strcmp(key, "wallpaper_top") == 0)
            set_color_str(c->s_wallpaper_top, sizeof(c->s_wallpaper_top), val);
        else if (strcmp(key, "wallpaper_bottom") == 0)
            set_color_str(c->s_wallpaper_bottom, sizeof(c->s_wallpaper_bottom),
                          val);
        else if (strcmp(key, "title_bg") == 0)
            set_color_str(c->s_title_bg, sizeof(c->s_title_bg), val);
        else if (strcmp(key, "title_bg_inactive") == 0)
            set_color_str(c->s_title_bg_inactive,
                          sizeof(c->s_title_bg_inactive), val);
        else if (strcmp(key, "title_fg") == 0)
            set_color_str(c->s_title_fg, sizeof(c->s_title_fg), val);
        else if (strcmp(key, "title_fg_inactive") == 0)
            set_color_str(c->s_title_fg_inactive,
                          sizeof(c->s_title_fg_inactive), val);
        else if (strcmp(key, "title_hover") == 0)
            set_color_str(c->s_title_hover, sizeof(c->s_title_hover), val);
        else if (strcmp(key, "title_close_hover") == 0)
            set_color_str(c->s_close_hover, sizeof(c->s_close_hover), val);
        else if (strcmp(key, "button_hover") == 0)
            set_color_str(c->s_btn_hover, sizeof(c->s_btn_hover), val);
        else
            log_msg("config %s:%d: unknown key '%s'", path, lineno, key);
    }

    fclose(f);
    return 0;
}

int config_resolve_colors(Config *c)
{
    c->col_background = parse_color(c->s_background, 0x0e1117);
    c->col_wallpaper_top = parse_color(c->s_wallpaper_top, 0x1b2436);
    c->col_wallpaper_bottom = parse_color(c->s_wallpaper_bottom, 0x0b0d12);
    c->col_border_focus = parse_color(c->s_border_focus, 0x3daee9);
    c->col_border_unfocus = parse_color(c->s_border_unfocus, 0x23262c);
    c->col_title_bg = parse_color(c->s_title_bg, 0x2b303a);
    c->col_title_bg_inactive = parse_color(c->s_title_bg_inactive, 0x22252c);
    c->col_title_fg = parse_color(c->s_title_fg, 0xeaeef3);
    c->col_title_fg_inactive = parse_color(c->s_title_fg_inactive, 0x8b93a1);
    c->col_title_hover = parse_color(c->s_title_hover, 0x39414d);
    c->col_close_hover = parse_color(c->s_close_hover, 0xd32f2f);
    c->col_panel_bg = parse_color(c->s_panel_bg, 0x171a20);
    c->col_panel_fg = parse_color(c->s_panel_fg, 0xc9d1db);
    c->col_btn_active = parse_color(c->s_btn_active, 0x2f5f8a);
    c->col_btn_inactive = parse_color(c->s_btn_inactive, 0x24272e);
    c->col_btn_hover = parse_color(c->s_btn_hover, 0x3a414c);
    c->col_menu_bg = parse_color(c->s_menu_bg, 0x1c1f26);
    c->col_menu_fg = parse_color(c->s_menu_fg, 0xd5dae2);
    c->col_menu_sel = parse_color(c->s_menu_sel, 0x2d5f8f);
    return 0;
}

void config_add_defaults_menu(Config *c)
{
    int n = 0;

    if (c->menu_count > 0)
        return;

    str_copy(c->menu[n].label, sizeof(c->menu[n].label), "Terminal");
    str_copy(c->menu[n].cmd, sizeof(c->menu[n].cmd), "@terminal");
    n++;
    str_copy(c->menu[n].label, sizeof(c->menu[n].label), "Editor");
    str_copy(c->menu[n].cmd, sizeof(c->menu[n].cmd), "@editor");
    n++;
    str_copy(c->menu[n].label, sizeof(c->menu[n].label), "Files");
    str_copy(c->menu[n].cmd, sizeof(c->menu[n].cmd), "@files");
    n++;
    str_copy(c->menu[n].label, sizeof(c->menu[n].label), "Web Browser");
    str_copy(c->menu[n].cmd, sizeof(c->menu[n].cmd), "@browser");
    n++;
    c->menu_count = n;
}

/*
 * Persist the wallpaper image path: rewrite the wallpaper= line of the
 * active config file (append it when missing).  Returns 0 on success.
 */
int config_set_wallpaper(const char *path)
{
    char line[640];
    char tmp[600];
    FILE *in, *out;
    int replaced = 0;

    if (!path || !*path || !cfg.config_path[0])
        return -1;

    str_copy(cfg.wallpaper, sizeof(cfg.wallpaper), path);

    snprintf(tmp, sizeof(tmp), "%s.tmp", cfg.config_path);
    in = fopen(cfg.config_path, "r");
    out = fopen(tmp, "w");
    if (!out) {
        if (in)
            fclose(in);
        return -1;
    }
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (strncmp(line, "wallpaper=", 10) == 0) {
                fprintf(out, "wallpaper=%s\n", path);
                replaced = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }
    if (!replaced)
        fprintf(out, "wallpaper=%s\n", path);
    fclose(out);
    if (rename(tmp, cfg.config_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
