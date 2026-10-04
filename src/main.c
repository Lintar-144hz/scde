/*
 * SCDE - Solo Coding Desktop Environment
 * X11 connection, startup/shutdown and the single event loop.
 */
#include "scde.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

Scde sc;
Config cfg;

static int x_error_code;
static int startup_phase;
static int another_wm;

/* --------------------------------------------------------------- errors -- */

static int on_xerror(Display *dpy, XErrorEvent *ev)
{
    char msg[256];

    (void)dpy;
    x_error_code = ev->error_code;

    if (startup_phase && ev->error_code == BadAccess)
        another_wm = 1;

    XGetErrorText(sc.dpy, ev->error_code, msg, sizeof(msg));
    /* a client that vanished between two events is completely normal */
    if (ev->error_code != BadWindow && ev->error_code != BadDrawable)
        log_msg("X error: %s (request %d.%d, resource 0x%lx)", msg,
                ev->request_code, ev->minor_code, (unsigned long)ev->resourceid);
    return 0;
}

static int trap_xerrors(void)
{
    x_error_code = 0;
    XSync(sc.dpy, False);
    return x_error_code;
}

/* -------------------------------------------------------------- signals -- */

static void on_signal(int sig)
{
    (void)sig;
    sc.running = 0;
}

static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                        /* no SA_RESTART: poll() wakes up */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);               /* no zombies from spawned apps */
}

/* ----------------------------------------------------------------- lock -- */

static int acquire_lock(void)
{
    const char *home = getenv("HOME");
    const char *cache;
    char base[400];
    struct flock fl;
    int fd;

    if (!home || !*home)
        home = "/tmp";

    cache = getenv("XDG_CACHE_HOME");
    if (cache && *cache) {
        snprintf(base, sizeof(base), "%s", cache);
    } else {
        snprintf(base, sizeof(base), "%s/.cache", home);
    }
    if (mkdir(base, 0700) != 0 && errno != EEXIST) {
        log_msg("cannot create cache dir %s: %s", base, strerror(errno));
        return -2;
    }
    snprintf(sc.lock_path, sizeof(sc.lock_path), "%s/scde", base);
    if (mkdir(sc.lock_path, 0700) != 0 && errno != EEXIST) {
        log_msg("cannot create dir %s: %s", sc.lock_path, strerror(errno));
        return -2;
    }
    snprintf(sc.lock_path, sizeof(sc.lock_path), "%s/scde/scde.lock", base);

    fd = open(sc.lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        log_msg("cannot open lock file %s: %s", sc.lock_path, strerror(errno));
        return -2;
    }

    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        close(fd);
        return -1;
    }

    if (ftruncate(fd, 0) == 0) {
        char pid[32];
        int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
        ssize_t wr = write(fd, pid, (size_t)n);
        (void)wr;                           /* informational only */
    }
    sc.lock_fd = fd;
    return 0;
}

/* ------------------------------------------------------------ font/atoms -- */

static void load_font(void)
{
    static const char *fallbacks[] = {
        "-*-fixed-medium-r-*-*-13-*-*-*-*-*-*-*",
        "9x15", "7x13", "fixed", NULL
    };
    int i;

    if (cfg.font[0]) {
        sc.font = XLoadQueryFont(sc.dpy, cfg.font);
        if (sc.font)
            return;
        log_msg("font '%s' unavailable, trying fallbacks", cfg.font);
    }
    for (i = 0; fallbacks[i]; i++) {
        sc.font = XLoadQueryFont(sc.dpy, fallbacks[i]);
        if (sc.font) {
            log_msg("using font '%s'", fallbacks[i]);
            return;
        }
    }
    log_msg("no core font found - text will not be drawn");
    sc.font = NULL;
    sc.font_ascent = 10;
    sc.font_descent = 3;
    sc.font_height = 13;
}

static void setup_font_metrics(void)
{
    if (sc.font) {
        sc.font_ascent = sc.font->ascent;
        sc.font_descent = sc.font->descent;
        sc.font_height = sc.font->ascent + sc.font->descent;
    }
}

static void setup_atoms(void)
{
    sc.atom_wm_protocols = XInternAtom(sc.dpy, "WM_PROTOCOLS", False);
    sc.atom_wm_delete = XInternAtom(sc.dpy, "WM_DELETE_WINDOW", False);
    sc.atom_wm_state = XInternAtom(sc.dpy, "WM_STATE", False);
    sc.atom_net_active = XInternAtom(sc.dpy, "_NET_ACTIVE_WINDOW", False);
    sc.atom_net_client_list =
        XInternAtom(sc.dpy, "_NET_CLIENT_LIST", False);
    sc.atom_net_wm_name = XInternAtom(sc.dpy, "_NET_WM_NAME", False);
    sc.atom_utf8_string = XInternAtom(sc.dpy, "UTF8_STRING", False);
    sc.atom_net_wm_state = XInternAtom(sc.dpy, "_NET_WM_STATE", False);
    sc.atom_net_wm_state_fullscreen =
        XInternAtom(sc.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    sc.atom_net_supported = XInternAtom(sc.dpy, "_NET_SUPPORTED", False);
    sc.atom_net_supporting_wm_check =
        XInternAtom(sc.dpy, "_NET_SUPPORTING_WM_CHECK", False);
}

/* announce the EWMH hints we implement (lets tools such as xdotool see us) */
static void setup_ewmh(void)
{
    Atom supported[8];
    int n = 0;
    const char *name = "scde";

    sc.wm_check = XCreateSimpleWindow(sc.dpy, sc.root, -10, -10, 1, 1, 0, 0, 0);
    XChangeProperty(sc.dpy, sc.wm_check, sc.atom_net_wm_name,
                    sc.atom_utf8_string, 8, PropModeReplace,
                    (const unsigned char *)name, (int)strlen(name));
    XChangeProperty(sc.dpy, sc.wm_check, sc.atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&sc.wm_check, 1);

    supported[n++] = sc.atom_net_active;
    supported[n++] = sc.atom_net_client_list;
    supported[n++] = sc.atom_net_wm_name;
    supported[n++] = sc.atom_net_wm_state;
    supported[n++] = sc.atom_net_wm_state_fullscreen;
    supported[n++] = sc.atom_net_supported;
    supported[n++] = sc.atom_net_supporting_wm_check;

    XChangeProperty(sc.dpy, sc.root, sc.atom_net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported, n);
    XChangeProperty(sc.dpy, sc.root, sc.atom_net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&sc.wm_check, 1);
}

static void setup_cursors(void)
{
    trap_xerrors();
    sc.cur_normal = XCreateFontCursor(sc.dpy, XC_left_ptr);
    sc.cur_move = XCreateFontCursor(sc.dpy, XC_fleur);
    sc.cur_resize = XCreateFontCursor(sc.dpy, XC_bottom_right_corner);
    if (trap_xerrors()) {
        sc.cur_normal = None;
        sc.cur_move = None;
        sc.cur_resize = None;
        log_msg("cursor font unavailable, using server default");
    }
    XDefineCursor(sc.dpy, sc.root, sc.cur_normal);
}

/* ------------------------------------------------------------- wallpaper -- */

static Pixmap wallpaper_pm;

static void setup_wallpaper(void)
{
    int r1, g1, b1, r2, g2, b2, y;
    GC wgc;

    if (!color_rgb(cfg.s_wallpaper_top, &r1, &g1, &b1) ||
        !color_rgb(cfg.s_wallpaper_bottom, &r2, &g2, &b2)) {
        XSetWindowBackground(sc.dpy, sc.root, cfg.col_background);
        XClearWindow(sc.dpy, sc.root);
        return;
    }

    wallpaper_pm = XCreatePixmap(sc.dpy, sc.root, (unsigned)sc.sw,
                                 (unsigned)sc.sh, (unsigned)sc.depth);
    if (wallpaper_pm == None)
        return;
    wgc = XCreateGC(sc.dpy, wallpaper_pm, 0, NULL);

    for (y = 0; y < sc.sh; y++) {
        int r = r1 + ((r2 - r1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        int g = g1 + ((g2 - g1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        int b = b1 + ((b2 - b1) * y) / (sc.sh > 1 ? sc.sh - 1 : 1);
        unsigned long px;

        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        px = (unsigned long)r << 16 | (unsigned long)g << 8 | (unsigned long)b;
        XSetForeground(sc.dpy, wgc, px);
        XFillRectangle(sc.dpy, wallpaper_pm, wgc, 0, y, (unsigned)sc.sw, 1);
    }
    XFreeGC(sc.dpy, wgc);
    XSetWindowBackgroundPixmap(sc.dpy, sc.root, wallpaper_pm);
    XClearWindow(sc.dpy, sc.root);
    XFlush(sc.dpy);
    wallpaper_apply();                  /* image wallpaper overrides gradient */
}

/* --------------------------------------------------------------- startup -- */

static void setup_root(void)
{
    XSetWindowAttributes attrs;

    XSetErrorHandler(on_xerror);

    attrs.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                       ButtonPressMask | KeyPressMask | PropertyChangeMask |
                       StructureNotifyMask;
    XChangeWindowAttributes(sc.dpy, sc.root, CWEventMask, &attrs);

    startup_phase = 1;
    XSync(sc.dpy, False);
    startup_phase = 0;

    if (another_wm) {
        char dpy_name[256];
        str_copy(dpy_name, sizeof(dpy_name), DisplayString(sc.dpy));
        close(sc.lock_fd);
        unlink(sc.lock_path);
        XCloseDisplay(sc.dpy);
        sc.dpy = NULL;
        die("another window manager is already running on display %s",
            dpy_name);
    }

    XSetWindowBackground(sc.dpy, sc.root, cfg.col_background);
    XClearWindow(sc.dpy, sc.root);

    sc.gc = XCreateGC(sc.dpy, sc.root, 0, NULL);
    input_grab_keys();
    input_grab_buttons();
}

/* ----------------------------------------------------------------- loop -- */

static void dispatch(XEvent *ev)
{
    switch (ev->type) {
    case MapRequest:
        handle_map_request(&ev->xmaprequest);
        break;
    case ConfigureRequest:
        handle_configure_request(&ev->xconfigurerequest);
        break;
    case DestroyNotify:
        handle_destroy_notify(&ev->xdestroywindow);
        break;
    case UnmapNotify:
        handle_unmap_notify(&ev->xunmap);
        break;
    case EnterNotify:
        handle_enter_notify(&ev->xcrossing);
        break;
    case KeyPress:
        handle_key_press(&ev->xkey);
        break;
    case ButtonPress:
        handle_button_press(&ev->xbutton);
        break;
    case ButtonRelease:
        break;
    case MotionNotify:
        if (msgbox_motion(&ev->xmotion))
            break;
        if (fm_motion(&ev->xmotion))
            break;
        if (ev->xmotion.window == sc.panel)
            panel_motion(&ev->xmotion);
        else if (ev->xmotion.window == sc.menu)
            menu_motion(&ev->xmotion);
        else
            handle_frame_motion(&ev->xmotion);
        break;
    case Expose:
        handle_expose(&ev->xexpose);
        break;
    case PropertyNotify:
        handle_property_notify(&ev->xproperty);
        break;
    case ClientMessage:
        handle_client_message(&ev->xclient);
        break;
    case MappingNotify:
        handle_mapping_notify(&ev->xmapping);
        break;
    case FocusIn:
        /* some clients try to steal focus - keep ours if the pointer is
           sitting in one of our managed windows */
        break;
    default:
        break;
    }
}

static void event_loop(void)
{
    struct pollfd pfd;

    pfd.fd = ConnectionNumber(sc.dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (sc.running) {
        if (XPending(sc.dpy) > 0) {
            XEvent ev;
            XNextEvent(sc.dpy, &ev);
            dispatch(&ev);
            XFlush(sc.dpy);
            continue;
        }

        int timeout = panel_timeout_ms();
        int rc = poll(&pfd, 1, timeout);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            log_msg("poll failed: %s", strerror(errno));
            break;
        }
        panel_tick();
    }
}

/* -------------------------------------------------------------- shutdown -- */

static void cleanup(void)
{
    menu_hide();
    cmd_hide();
    fm_cleanup();

    while (sc.clients) {
        Client *c = sc.clients;

        sc.clients = c->next;
        if (c->frame != None) {
            XSelectInput(sc.dpy, c->win, NoEventMask);
            XReparentWindow(sc.dpy, c->win, sc.root, c->x + c->bw,
                            c->y + c->bw + c->title_h);
            XRemoveFromSaveSet(sc.dpy, c->win);
            XDestroyWindow(sc.dpy, c->frame);
        }
        free(c);
    }
    sc.focused = NULL;
    apps_free();

    XUngrabKey(sc.dpy, AnyKey, AnyModifier, sc.root);
    XUngrabButton(sc.dpy, AnyButton, AnyModifier, sc.root);
    XUngrabKeyboard(sc.dpy, CurrentTime);
    XUngrabPointer(sc.dpy, CurrentTime);

    panel_destroy();
    if (sc.menu != None) {
        XDestroyWindow(sc.dpy, sc.menu);
        sc.menu = None;
    }
    if (sc.cmd != None) {
        XDestroyWindow(sc.dpy, sc.cmd);
        sc.cmd = None;
    }
    if (sc.wm_check != None) {
        XDestroyWindow(sc.dpy, sc.wm_check);
        sc.wm_check = None;
    }

    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_client_list);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_active);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_supported);
    XDeleteProperty(sc.dpy, sc.root, sc.atom_net_supporting_wm_check);

    if (wallpaper_pm != None) {
        XFreePixmap(sc.dpy, wallpaper_pm);
        wallpaper_pm = None;
    }
    if (sc.gc)
        XFreeGC(sc.dpy, sc.gc);
    if (sc.font)
        XFreeFont(sc.dpy, sc.font);
    if (sc.cur_normal)
        XFreeCursor(sc.dpy, sc.cur_normal);
    if (sc.cur_move)
        XFreeCursor(sc.dpy, sc.cur_move);
    if (sc.cur_resize)
        XFreeCursor(sc.dpy, sc.cur_resize);

    XSync(sc.dpy, False);
    XCloseDisplay(sc.dpy);
    sc.dpy = NULL;

    if (sc.lock_fd >= 0) {
        unlink(sc.lock_path);
        close(sc.lock_fd);
        sc.lock_fd = -1;
    }
    log_msg("clean shutdown");
}

/* ------------------------------------------------------------------ main -- */

static void usage(int code)
{
    printf("SCDE %s - Solo Coding Desktop Environment\n", SCDE_VERSION);
    printf("usage: scde [-c config] [-v] [-h]\n\n");
    printf("  -c FILE   read configuration from FILE\n"
           "            (default: ~/.config/scde/config)\n"
           "  -v        print version and exit\n"
           "  -h        show this help\n\n");
    printf("bindings:\n"
           "  Alt+F4        close window          Alt+Tab      switch windows\n"
           "  Alt+F2        run a command         Alt+Space    application menu\n"
           "  Alt+L-drag    move window           Alt+R-drag   resize window\n"
           "  Alt+Enter     start terminal        Alt+E        file manager\n"
           "  Alt+Up        maximize              Alt+Down     minimize\n"
           "  Alt+Left/Right  snap half screen    Alt+D        show desktop\n"
           "  right click desktop   desktop menu\n");
    exit(code);
}

int main(int argc, char **argv)
{
    char default_path[512];
    const char *home;
    const char *conf = NULL;
    int opt;

    memset(&sc, 0, sizeof(sc));
    sc.lock_fd = -1;
    sc.running = 1;

    while ((opt = getopt(argc, argv, "c:vh")) != -1) {
        switch (opt) {
        case 'c':
            conf = optarg;
            break;
        case 'v':
            printf("scde %s\n", SCDE_VERSION);
            return 0;
        case 'h':
            usage(0);
            return 0;
        default:
            usage(1);
            return 1;
        }
    }

    setup_signals();
    config_defaults(&cfg);

    home = getenv("HOME");
    if (!home)
        home = "";
    snprintf(default_path, sizeof(default_path), "%s/.config/scde/config", home);
    str_copy(cfg.config_path, sizeof(cfg.config_path),
             conf ? conf : default_path);

    if (config_load(&cfg, cfg.config_path) != 0) {
        if (conf)
            die("config file not found: %s", cfg.config_path);
        log_msg("no config at %s, using defaults", cfg.config_path);
    } else {
        log_msg("loaded config %s", cfg.config_path);
    }
    config_add_defaults_menu(&cfg);

    {
        int lock_rc = acquire_lock();
        if (lock_rc == -1)
            die("another SCDE instance is already running (lock %s)",
                sc.lock_path);
        if (lock_rc != 0)
            die("cannot create lock file %s", sc.lock_path);
    }

    sc.dpy = XOpenDisplay(NULL);
    if (!sc.dpy) {
        if (sc.lock_fd >= 0) {
            unlink(sc.lock_path);
            close(sc.lock_fd);
        }
        die("cannot open display '%s'",
            getenv("DISPLAY") ? getenv("DISPLAY") : "");
    }

    sc.screen = DefaultScreen(sc.dpy);
    sc.root = RootWindow(sc.dpy, sc.screen);
    sc.sw = DisplayWidth(sc.dpy, sc.screen);
    sc.sh = DisplayHeight(sc.dpy, sc.screen);
    sc.depth = DefaultDepth(sc.dpy, sc.screen);
    sc.cmap = DefaultColormap(sc.dpy, sc.screen);

    XSetErrorHandler(on_xerror);

    config_resolve_colors(&cfg);
    load_font();
    setup_font_metrics();
    setup_atoms();
    setup_cursors();
    setup_root();
    setup_ewmh();
    setup_wallpaper();

    log_msg("SCDE %s started on %dx%d (display %s)", SCDE_VERSION, sc.sw,
            sc.sh, DisplayString(sc.dpy));

    apps_scan();
    panel_create();
    manage_existing();
    panel_update();

    event_loop();

    cleanup();
    return 0;
}
