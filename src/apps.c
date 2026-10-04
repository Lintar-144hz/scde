/*
 * SCDE - installed applications (.desktop scan) and browser detection
 */
#include "scde.h"

#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_APPS 512

/* remove X-Desktop-Entry field codes (%f, %u, ...) from an Exec line */
static void clean_exec(char *out, size_t size, const char *exec)
{
    size_t oi = 0;
    const char *p = exec;

    while (*p) {
        const char *start = p;
        size_t len;

        while (*p && *p != ' ' && *p != '\t')
            p++;
        len = (size_t)(p - start);
        if (len > 0 && !(len <= 2 && start[0] == '%')) {
            if (oi + len + 2 < size) {
                if (oi)
                    out[oi++] = ' ';
                memcpy(out + oi, start, len);
                oi += len;
            }
        }
        while (*p == ' ' || *p == '\t')
            p++;
    }
    out[oi] = '\0';
}

static int read_desktop(const char *path, MenuEntry *entry)
{
    FILE *f;
    char line[512];
    char name[MAX_MENU_LABEL];
    char exec[MAX_MENU_CMD];
    int in_entry = 0, have_type = 0, terminal = 0, skip = 0;
    int have_name = 0, have_exec = 0;

    f = fopen(path, "r");
    if (!f)
        return 0;
    name[0] = '\0';
    exec[0] = '\0';

    while (fgets(line, sizeof(line), f)) {
        char *s = str_trim(line);
        char *eq;

        if (*s == '#' || *s == '\0')
            continue;
        if (s[0] == '[') {
            if (strcmp(s, "[Desktop Entry]") == 0) {
                in_entry = 1;
            } else if (in_entry) {
                break;                  /* only the first group */
            }
            continue;
        }
        if (!in_entry)
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        {
            char *key = str_trim(s);
            char *val = str_trim(eq + 1);

            if (strcmp(key, "Name") == 0 && !have_name) {
                str_copy(name, sizeof(name), val);
                have_name = 1;
            } else if (strcmp(key, "Exec") == 0 && !have_exec) {
                str_copy(exec, sizeof(exec), val);
                have_exec = 1;
            } else if (strcmp(key, "Type") == 0) {
                have_type = 1;
                if (strcmp(val, "Application") != 0)
                    skip = 1;
            } else if (strcmp(key, "Terminal") == 0) {
                terminal = (strcmp(val, "true") == 0);
            } else if (strcmp(key, "Hidden") == 0 ||
                       strcmp(key, "NoDisplay") == 0) {
                if (strcmp(val, "true") == 0)
                    skip = 1;
            }
        }
    }
    fclose(f);

    if (skip || !have_name || !have_exec)
        return 0;
    (void)have_type;

    str_copy(entry->label, sizeof(entry->label), name);
    if (terminal) {
        char cleaned[MAX_MENU_CMD];
        char joined[MAX_MENU_CMD * 2 + 32];

        clean_exec(cleaned, sizeof(cleaned), exec);
        snprintf(joined, sizeof(joined), "%s -e %s", apps_terminal(), cleaned);
        str_copy(entry->cmd, sizeof(entry->cmd), joined);
    } else {
        clean_exec(entry->cmd, sizeof(entry->cmd), exec);
    }
    return 1;
}

static int entry_exists(const MenuEntry *list, int n, const char *label)
{
    int i;

    for (i = 0; i < n; i++)
        if (strcasecmp(list[i].label, label) == 0)
            return 1;
    return 0;
}

static void scan_dir(const char *dir, MenuEntry *list, int *n, int max)
{
    DIR *d;
    struct dirent *e;

    d = opendir(dir);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && *n < max) {
        MenuEntry tmp;
        size_t len = strlen(e->d_name);
        char path[768];

        if (len < 9 || strcmp(e->d_name + len - 8, ".desktop") != 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!read_desktop(path, &tmp))
            continue;
        if (tmp.cmd[0] == '\0' || tmp.label[0] == '\0')
            continue;
        if (entry_exists(list, *n, tmp.label))
            continue;
        list[*n] = tmp;
        (*n)++;
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b)
{
    const MenuEntry *ea = a, *eb = b;

    return strcasecmp(ea->label, eb->label);
}

/* ------------------------------------------------------- PATH detection -- */

static int path_lookup(const char *name, char *out, size_t size)
{
    const char *path;
    char dirs[1024];
    char *p, *save;

    if (!name || !*name)
        return 0;
    if (strchr(name, '/'))                 /* absolute or relative path */
        return access(name, X_OK) == 0 ? (str_copy(out, size, name), 1) : 0;

    path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    str_copy(dirs, sizeof(dirs), path);
    for (p = strtok_r(dirs, ":", &save); p; p = strtok_r(NULL, ":", &save)) {
        char full[512];

        snprintf(full, sizeof(full), "%s/%s", p, name);
        if (access(full, X_OK) == 0) {
            str_copy(out, size, name);
            return 1;
        }
    }
    return 0;
}

/* first entry of names[] that is executable, or NULL */
static const char *pick(const char *const *names)
{
    static char found[128];
    int i;

    for (i = 0; names[i]; i++)
        if (path_lookup(names[i], found, sizeof(found)))
            return found;
    return NULL;
}

const char *apps_terminal(void)
{
    static const char *names[] = {
        "x-terminal-emulator", "xterm", "uxterm", "urxvt", "rxvt", "st",
        "aterm", "xfce4-terminal", "gnome-terminal", "konsole", "lxterminal",
        "sakura", "qterminal", "mate-terminal", "terminator", "tilix",
        "alacritty", "kitty", "foot", NULL
    };
    static char cached[256];
    static int done;

    if (!done) {
        const char *f;

        done = 1;
        cached[0] = '\0';
        if (cfg.terminal[0]) {
            if (strpbrk(cfg.terminal, " \t") != NULL ||
                path_lookup(cfg.terminal, cached, sizeof(cached)))
                str_copy(cached, sizeof(cached), cfg.terminal);
        } else if ((f = pick(names)) != NULL) {
            str_copy(cached, sizeof(cached), f);
        }
        if (!cached[0])
            log_msg("no terminal found (install xterm or set terminal=)");
    }
    return cached;
}

const char *apps_editor(void)
{
    static const char *gui[] = {
        "gedit", "gnome-text-editor", "mousepad", "leafpad", "xed", "kate",
        "kwrite", "geany", "code", "sublime_text", NULL
    };
    static const char *cli[] = { "vi", "vim", "nvim", "nano", "micro", NULL };
    static char cached[256];
    static int done;

    if (!done) {
        const char *env = getenv("VISUAL");
        const char *f;

        done = 1;
        cached[0] = '\0';
        if (!env || !*env)
            env = getenv("EDITOR");
        if (env && *env) {
            char tmp[128];
            if (path_lookup(env, tmp, sizeof(tmp)))
                str_copy(cached, sizeof(cached), env);
        }
        if (!cached[0] && (f = pick(gui)) != NULL)
            str_copy(cached, sizeof(cached), f);
        if (!cached[0] && (f = pick(cli)) != NULL)
            str_copy(cached, sizeof(cached), f);
    }
    return cached;
}

const char *apps_filemanager(void)
{
    static const char *gui[] = {
        "pcmanfm", "thunar", "nautilus", "dolphin", "nemo", "caja", "rox",
        "xfe", "spacefm", "pcmanfm-qt", NULL
    };
    static const char *cli[] = { "mc", "ranger", "vifm", "nnn", "lf", NULL };
    static char cached[256];
    static int done;

    if (!done) {
        const char *f;

        done = 1;
        cached[0] = '\0';
        if ((f = pick(gui)) != NULL)
            str_copy(cached, sizeof(cached), f);
        else if ((f = pick(cli)) != NULL)
            str_copy(cached, sizeof(cached), f);
    }
    return cached;
}

const char *apps_imageviewer(void)
{
    static const char *names[] = {
        "feh", "nsxiv", "sxiv", "display", "eog", "ristretto", "gpicview",
        NULL
    };
    static char cached[256];
    static int done;

    if (!done) {
        const char *f;

        done = 1;
        cached[0] = '\0';
        if ((f = pick(names)) != NULL)
            str_copy(cached, sizeof(cached), f);
    }
    return cached;
}

const char *apps_mediaplayer(void)
{
    static const char *names[] = {
        "mpv", "vlc", "mplayer", "ffplay", NULL
    };
    static char cached[256];
    static int done;

    if (!done) {
        const char *f;

        done = 1;
        cached[0] = '\0';
        if ((f = pick(names)) != NULL)
            str_copy(cached, sizeof(cached), f);
    }
    return cached;
}

int apps_have(const char *name)
{
    char tmp[128];

    return path_lookup(name, tmp, sizeof(tmp));
}

/* apply the configured wallpaper image (needs feh); no-op when empty */
void wallpaper_apply(void)
{
    char q[600];
    char cmd[700];

    if (!cfg.wallpaper[0] || access(cfg.wallpaper, R_OK) != 0)
        return;
    if (!apps_have("feh"))
        return;
    shell_quote(q, sizeof(q), cfg.wallpaper);
    snprintf(cmd, sizeof(cmd), "feh --no-fehbg --bg-fill %s", q);
    spawn(cmd);
}

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');

    if (!dot || (slash && dot < slash))
        return "";
    return dot + 1;
}

static int ext_in(const char *ext, const char *const *list)
{
    int i;

    for (i = 0; list[i]; i++)
        if (strcasecmp(ext, list[i]) == 0)
            return 1;
    return 0;
}

int apps_file_kind(const char *path)
{
    static const char *const images[] = {
        "jpg", "jpeg", "png", "gif", "bmp", "svg", "webp", "ico", "tif",
        "tiff", "ppm", "pgm", "xbm", "xpm", "avif", "jxl", NULL
    };
    static const char *const videos[] = {
        "mp4", "mkv", "webm", "avi", "mov", "m4v", "mpg", "mpeg", "flv",
        "ts", "mts", "wmv", "3gp", "ogv", "m2ts", NULL
    };
    static const char *const audios[] = {
        "mp3", "flac", "ogg", "oga", "wav", "m4a", "aac", "opus", "wma",
        "ape", "mid", "midi", "mka", "alac", NULL
    };
    static const char *const texts[] = {
        "txt", "md", "log", "c", "h", "cpp", "hpp", "py", "sh", "conf",
        "cfg", "ini", "json", "xml", "csv", "patch", "diff", "desktop",
        "yml", "yaml", "toml", "cmake", "mk", NULL
    };
    static const char *const archives[] = {
        "zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar", "lz", "zst",
        "jar", "deb", "rpm", "iso", NULL
    };
    const char *ext;

    if (!path || !*path)
        return F_KIND_OTHER;
    ext = ext_of(path);
    if (ext_in(ext, images))
        return F_KIND_IMAGE;
    if (ext_in(ext, videos))
        return F_KIND_VIDEO;
    if (ext_in(ext, audios))
        return F_KIND_AUDIO;
    if (ext_in(ext, texts))
        return F_KIND_TEXT;
    if (ext_in(ext, archives))
        return F_KIND_ARCHIVE;
    if (ext_in(ext, (const char *const[]){"html", "htm", "xhtml", NULL}))
        return F_KIND_HTML;
    return F_KIND_OTHER;
}

/* open a local file with the right application for its type */
void apps_open_file(const char *path)
{
    char quoted[768];
    char url[720];
    char cmd[900];
    const char *prog;
    int kind;

    if (!path || !*path)
        return;
    kind = apps_file_kind(path);
    shell_quote(quoted, sizeof(quoted), path);

    if (kind == F_KIND_IMAGE || kind == F_KIND_VIDEO ||
        kind == F_KIND_AUDIO) {
        prog = (kind == F_KIND_IMAGE) ? apps_imageviewer()
                                      : apps_mediaplayer();
        if (prog && *prog) {
            snprintf(cmd, sizeof(cmd), "%s %s", prog, quoted);
            spawn(cmd);
            return;
        }
        msgbox_show("No player found",
                    kind == F_KIND_IMAGE
                        ? "Install an image viewer:\napt install feh"
                        : "Install a media player:\napt install mpv");
        return;
    }
    /* everything else: xdg-open, then the browser as a last resort */
    prog = pick((const char *const[]){"xdg-open", NULL});
    if (prog && *prog) {
        snprintf(cmd, sizeof(cmd), "%s %s", prog, quoted);
        spawn(cmd);
        return;
    }
    snprintf(url, sizeof(url), "file://%s", path);
    shell_quote(quoted, sizeof(quoted), url);
    snprintf(cmd, sizeof(cmd), "%s %s", apps_browser(), quoted);
    spawn(cmd);
}

const char *apps_browser(void)
{
    static const char *names[] = {
        "firefox", "firefox-esr", "chromium", "chromium-browser",
        "google-chrome", "google-chrome-stable", "brave-browser",
        "falkon", "midori", "epiphany", "xdg-open", NULL
    };
    const char *f;

    if (sc.browser[0])
        return sc.browser;
    if (cfg.browser[0]) {
        str_copy(sc.browser, sizeof(sc.browser), cfg.browser);
        return sc.browser;
    }
    f = pick(names);
    str_copy(sc.browser, sizeof(sc.browser), f ? f : "xdg-open");
    return sc.browser;
}

/* ---------------------------------------------------------------- actions -- */

void apps_run(const char *what, const char *cmd)
{
    char exe[256];

    if (!cmd || !*cmd)
        return;
    /* commands with shell syntax ($VAR, quotes, ...) are left to the shell */
    if (strpbrk(cmd, "$`\"'|&;<>()*?[]{}~") == NULL) {
        char *sp = strpbrk(cmd, " \t");
        size_t len = sp ? (size_t)(sp - cmd) : strlen(cmd);

        if (len >= sizeof(exe))
            len = sizeof(exe) - 1;
        memcpy(exe, cmd, len);
        exe[len] = '\0';
        if (!path_lookup(exe, exe, sizeof(exe))) {
            msgbox_show("Application not found",
                        "Install it, or point the matching key in\n"
                        "~/.config/scde/config at a program you have.");
            log_msg("%s not found: %s", what, exe);
            return;
        }
    }
    spawn(cmd);
}

void apps_run_terminal(const char *what)
{
    const char *term = apps_terminal();

    if (!term[0]) {
        msgbox_show("No terminal found",
                    "Install a terminal (apt install xterm) or set\n"
                    "terminal= in ~/.config/scde/config");
        return;
    }
    if (strcmp(what, "editor") == 0) {
        const char *ed = apps_editor();
        char inner[640];

        if (!ed[0]) {
            msgbox_show("No editor found",
                        "Install nano/vi or set EDITOR, or install a\n"
                        "graphical editor such as mousepad or gedit.");
            return;
        }
        {   /* graphical editors run directly, cli editors need the terminal */
            static const char *gui_names[] = {
                "gedit", "gnome-text-editor", "mousepad", "leafpad", "xed",
                "kate", "kwrite", "geany", "emacs", "code", "sublime_text",
                NULL
            };
            int i, is_gui = 0;

            for (i = 0; gui_names[i]; i++)
                if (strcmp(ed, gui_names[i]) == 0)
                    is_gui = 1;
            if (is_gui) {
                apps_run("editor", ed);
                return;
            }
        }
        snprintf(inner, sizeof(inner), "%s -e %s", term, ed);
        spawn(inner);
        return;
    }
    spawn(term);
}

void apps_run_files(void)
{
    const char *fm = apps_filemanager();
    char cmd[600];

    if (fm[0]) {
        static const char *gui_fm[] = {
            "pcmanfm", "thunar", "nautilus", "dolphin", "nemo", "caja",
            "rox", "xfe", "spacefm", "pcmanfm-qt", NULL
        };
        int i, is_gui = 0;

        for (i = 0; gui_fm[i]; i++)
            if (strcmp(fm, gui_fm[i]) == 0)
                is_gui = 1;
        if (is_gui) {
            snprintf(cmd, sizeof(cmd), "%s \"$HOME\"", fm);
            spawn(cmd);
            return;
        }
        if (apps_terminal()[0]) {
            snprintf(cmd, sizeof(cmd), "%s -e %s", apps_terminal(), fm);
            spawn(cmd);
            return;
        }
    }
    /* last resort: show $HOME in the browser */
    {
        const char *home = getenv("HOME");

        snprintf(cmd, sizeof(cmd), "%s 'file://%s'", apps_browser(),
                 home ? home : "/");
        spawn(cmd);
    }
}

/* build the launcher menu: pinned entries, separator, installed apps */
void apps_scan(void)
{
    static const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/usr/share/applications/applications-merged",
        NULL
    };
    const char *home = getenv("HOME");
    MenuEntry *apps;
    int n = 0, total, i;
    char user_dir[512];

    apps_free();

    apps = calloc(MAX_APPS, sizeof(MenuEntry));
    if (!apps)
        return;

    for (i = 0; dirs[i]; i++)
        scan_dir(dirs[i], apps, &n, MAX_APPS);
    if (home && *home) {
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications",
                 home);
        scan_dir(user_dir, apps, &n, MAX_APPS);
    }

    qsort(apps, (size_t)n, sizeof(MenuEntry), cmp_entry);

    total = cfg.menu_count + (n > 0 ? 1 + n : 0);
    sc.menu_items = calloc((size_t)(total > 0 ? total : 1), sizeof(MenuEntry));
    if (!sc.menu_items) {
        free(apps);
        return;
    }
    sc.menu_item_count = 0;

    for (i = 0; i < cfg.menu_count; i++)
        sc.menu_items[sc.menu_item_count++] = cfg.menu[i];
    if (n > 0) {
        str_copy(sc.menu_items[sc.menu_item_count].label,
                 sizeof(sc.menu_items[0].label), "-");
        sc.menu_items[sc.menu_item_count].cmd[0] = '\0';
        sc.menu_item_count++;
        for (i = 0; i < n; i++)
            sc.menu_items[sc.menu_item_count++] = apps[i];
    }

    free(apps);
    str_copy(sc.browser, sizeof(sc.browser), apps_browser());
    log_msg("launcher: %d menu entries (%d pinned, %d applications)",
            sc.menu_item_count, cfg.menu_count, n);
}

void apps_free(void)
{
    free(sc.menu_items);
    sc.menu_items = NULL;
    sc.menu_item_count = 0;
}
