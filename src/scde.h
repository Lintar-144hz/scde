/*
 * SCDE - Solo Coding Desktop Environment
 * V0.2 - lightweight Xlib window manager / desktop environment
 */
#ifndef SCDE_H
#define SCDE_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <signal.h>
#include <stddef.h>

#define SCDE_VERSION "0.2"
#define SCDE_NAME "scde"

#define MAX_MENU_ITEMS 64
#define MAX_MENU_LABEL 64
#define MAX_MENU_CMD 256
#define MAX_PANEL_BTNS 256
#define MIN_CLIENT_SIZE 48
#define NAME_MAX_LEN 128
#define TITLE_BTN_W 24

/* drag modes */
#define DRAG_MOVE      0
#define DRAG_RESIZE    1
#define DIR_UP         1
#define DIR_DOWN       2
#define DIR_LEFT       4
#define DIR_RIGHT      8
#define DRAG_DIR_MASK  15

/* title bar buttons */
#define BTN_NONE 0
#define BTN_MIN  1
#define BTN_MAX  2
#define BTN_CLOSE 3

/* ------------------------------------------------------------------ config */

typedef struct {
    char label[MAX_MENU_LABEL];
    char cmd[MAX_MENU_CMD];
} MenuEntry;

typedef struct {
    int panel_top;              /* 1 = top panel, 0 = bottom panel */
    int panel_height;
    int border_width;
    int title_height;
    int focus_follows_mouse;
    int show_clock;
    int show_tray;
    int show_desktop_btn;
    char clock_format[64];
    char clock_date_format[64];
    char terminal[128];
    char browser[128];
    char font[128];
    char config_path[512];
    char wallpaper[512];               /* image path, empty = gradient */

    /* colour strings (parsed after the display is open) */
    char s_background[32];
    char s_wallpaper_top[32];
    char s_wallpaper_bottom[32];
    char s_border_focus[32];
    char s_border_unfocus[32];
    char s_title_bg[32];
    char s_title_bg_inactive[32];
    char s_title_fg[32];
    char s_title_fg_inactive[32];
    char s_title_hover[32];
    char s_close_hover[32];
    char s_panel_bg[32];
    char s_panel_fg[32];
    char s_btn_active[32];
    char s_btn_inactive[32];
    char s_btn_hover[32];
    char s_menu_bg[32];
    char s_menu_fg[32];
    char s_menu_sel[32];

    unsigned long col_background;
    unsigned long col_wallpaper_top;
    unsigned long col_wallpaper_bottom;
    unsigned long col_border_focus;
    unsigned long col_border_unfocus;
    unsigned long col_title_bg;
    unsigned long col_title_bg_inactive;
    unsigned long col_title_fg;
    unsigned long col_title_fg_inactive;
    unsigned long col_title_hover;
    unsigned long col_close_hover;
    unsigned long col_panel_bg;
    unsigned long col_panel_fg;
    unsigned long col_btn_active;
    unsigned long col_btn_inactive;
    unsigned long col_btn_hover;
    unsigned long col_menu_bg;
    unsigned long col_menu_fg;
    unsigned long col_menu_sel;

    MenuEntry menu[MAX_MENU_ITEMS];     /* pinned entries from the config */
    int menu_count;
} Config;

/* ----------------------------------------------------------------- client */

typedef struct Client Client;
struct Client {
    Window win;                 /* application window */
    Window frame;               /* decoration frame */
    int x, y;                   /* frame position on screen */
    int w, h;                   /* client (content) size */
    int bw;                     /* frame border width */
    int title_h;
    int minimized;
    int maximized;
    int fullscreen;
    int sx, sy, sw, sh;         /* geometry saved while maximized */
    int hover_btn;
    int ignore_unmap;
    char name[NAME_MAX_LEN];
    Client *next;
};

/* ----------------------------------------------------------------- global */

typedef struct {
    Display *dpy;
    int screen;
    Window root;
    int sw, sh;
    int depth;
    Colormap cmap;
    Visual *visual;

    GC gc;
    XFontStruct *font;
    int font_ascent;
    int font_descent;
    int font_height;

    Atom atom_wm_protocols;
    Atom atom_wm_delete;
    Atom atom_wm_state;
    Atom atom_net_active;
    Atom atom_net_client_list;
    Atom atom_net_wm_name;
    Atom atom_utf8_string;
    Atom atom_net_wm_state;
    Atom atom_net_wm_state_fullscreen;
    Atom atom_net_supported;
    Atom atom_net_supporting_wm_check;
    Window wm_check;

    Client *clients;            /* mapping order, head = first mapped */
    Client *focused;
    int show_desktop;

    MenuEntry *menu_items;      /* pinned + installed applications */
    int menu_item_count;
    char browser[128];

    Window panel;
    int panel_w;
    Window menu;
    Window cmd;

    Cursor cur_move;
    Cursor cur_resize;
    Cursor cur_normal;

    unsigned int numlock_mask;
    int lock_fd;
    char lock_path[512];

    volatile sig_atomic_t running;
} Scde;

extern Scde sc;
extern Config cfg;

/* ------------------------------------------------------------------ util.c */

void log_msg(const char *fmt, ...);
void die(const char *fmt, ...);
void spawn(const char *cmd);
void shell_quote(char *out, size_t size, const char *s);
unsigned long parse_color(const char *str, unsigned long fallback);
char *str_trim(char *s);
void str_copy(char *dst, size_t size, const char *src);
int text_width(const char *s);
void fit_text(char *dst, size_t dst_size, const char *src, int max_w);
void draw_text(Window w, int x, int baseline_y, unsigned long color,
               const char *s);
void draw_text_clip(Window w, int x, int max_x, int baseline_y,
                    unsigned long color, const char *s);
int color_rgb(const char *str, int *r, int *g, int *b);

/* ---------------------------------------------------------------- config.c */

void config_defaults(Config *c);
int config_load(Config *c, const char *path);
int config_resolve_colors(Config *c);
void config_add_defaults_menu(Config *c);
int config_set_wallpaper(const char *path);

/* --------------------------------------------------------------- apps.c */

void apps_scan(void);
void apps_free(void);
const char *apps_browser(void);
const char *apps_terminal(void);
const char *apps_editor(void);
const char *apps_filemanager(void);
const char *apps_imageviewer(void);
const char *apps_mediaplayer(void);
int apps_have(const char *name);
void wallpaper_apply(void);
void apps_run(const char *what, const char *cmd);
void apps_run_terminal(const char *what);
void apps_run_files(void);
int apps_file_kind(const char *path);
void apps_open_file(const char *path);

/* file kinds for apps_file_kind() */
#define F_KIND_OTHER   0
#define F_KIND_IMAGE   1
#define F_KIND_VIDEO   2
#define F_KIND_AUDIO   3
#define F_KIND_TEXT    4
#define F_KIND_ARCHIVE 5
#define F_KIND_HTML    6

/* --------------------------------------------------------------- msgbox.c */

void msgbox_show(const char *title, const char *text);
void msgbox_hide(void);
int msgbox_open(void);
Window msgbox_window(void);
int msgbox_expose(XExposeEvent *ev);
int msgbox_button(XButtonEvent *ev);
int msgbox_motion(XMotionEvent *ev);
int msgbox_key(XKeyEvent *ev);

/* ------------------------------------------------------------------- wm.c */

Client *client_find(Window w);
Client *client_from_frame(Window frame);
int client_count(void);
int frame_w(Client *c);
int frame_h(Client *c);
void frame_sync(Client *c);
void frame_paint(Client *c);
void manage(Window w);
void unmanage(Client *c, int client_alive);
void focus_client(Client *c);
void unfocus_all(void);
void close_client(Client *c);
void cycle_windows(int dir);
void client_maximize(Client *c, int on);
void client_minimize(Client *c, int on);
void show_desktop_toggle(void);
void snap_client(Client *c, int side);
int frame_button_at(Client *c, int x, int y);
void handle_map_request(XMapRequestEvent *ev);
void handle_configure_request(XConfigureRequestEvent *ev);
void handle_destroy_notify(XDestroyWindowEvent *ev);
void handle_unmap_notify(XUnmapEvent *ev);
void handle_enter_notify(XCrossingEvent *ev);
void handle_property_notify(XPropertyEvent *ev);
void handle_client_message(XClientMessageEvent *ev);
void handle_expose(XExposeEvent *ev);
void handle_frame_button_press(XButtonEvent *ev);
void handle_frame_motion(XMotionEvent *ev);
void manage_existing(void);
void update_client_list(void);
void update_active_property(void);

/* ---------------------------------------------------------------- input.c */

void input_grab_keys(void);
void input_grab_buttons(void);
void handle_key_press(XKeyEvent *ev);
void handle_button_press(XButtonEvent *ev);
void handle_mapping_notify(XMappingEvent *ev);
void start_drag(Client *c, XButtonEvent *ev, int mode, int dirflags);
unsigned int strip_mods(unsigned int state);

/* ---------------------------------------------------------------- panel.c */

void panel_create(void);
void panel_destroy(void);
void panel_update(void);
void panel_raise(void);
void panel_expose(XExposeEvent *ev);
int panel_button_press(XButtonEvent *ev);
void panel_motion(XMotionEvent *ev);
void panel_tick(void);
int panel_timeout_ms(void);
int panel_reserved(void);

/* -------------------------------------------------------------- launcher.c */

void menu_show(void);
void menu_hide(void);
int menu_is_open(void);
void menu_key(XKeyEvent *ev);
void menu_pointer(XButtonEvent *ev);
void menu_motion(XMotionEvent *ev);
void menu_expose(XExposeEvent *ev);
void desktop_menu_show(XButtonEvent *ev);

void cmd_show(void);
void cmd_hide(void);
int cmd_is_open(void);
void cmd_key(XKeyEvent *ev);
void cmd_expose(XExposeEvent *ev);

/* ------------------------------------------------------------------- fm.c */

void fm_show(void);
void fm_hide(void);
int fm_open(void);
Window fm_window(void);
int fm_expose(XExposeEvent *ev);
int fm_button(XButtonEvent *ev);
int fm_motion(XMotionEvent *ev);
void fm_key(XKeyEvent *ev);
void fm_cleanup(void);

#endif /* SCDE_H */
