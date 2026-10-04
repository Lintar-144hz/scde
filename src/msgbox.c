/*
 * SCDE - small modal message box (used when a program is not installed)
 */
#include "scde.h"

#include <stdio.h>
#include <string.h>

#define MB_W 470
#define MB_PAD 16

static int mb_open;
static Window mb_win;
static int mb_x, mb_y, mb_w, mb_h;
static int mb_ok_x, mb_ok_y, mb_ok_w, mb_ok_h;
static int mb_hover_ok;
static char mb_title[96];
static char mb_lines[6][160];
static int mb_line_count;
static int mb_btn_h;

Window msgbox_window(void)
{
    return mb_win;
}

int msgbox_open(void)
{
    return mb_open;
}

static void msgbox_layout(void)
{
    int tw;

    mb_w = MB_W;
    if (mb_w > sc.sw - 40)
        mb_w = sc.sw - 40;
    mb_btn_h = sc.font_height + 14;
    if (mb_btn_h < 28)
        mb_btn_h = 28;
    mb_h = sc.font_height * (mb_line_count + 1) + mb_btn_h + 3 * MB_PAD;
    if (mb_h < 110)
        mb_h = 110;
    mb_x = (sc.sw - mb_w) / 2;
    mb_y = (sc.sh - mb_h) / 2;
    if (mb_x < 0)
        mb_x = 0;
    if (mb_y < 0)
        mb_y = 0;

    tw = text_width("OK") + 28;
    mb_ok_w = tw < 76 ? 76 : tw;
    mb_ok_h = mb_btn_h - 6;
    mb_ok_x = mb_w - MB_PAD - mb_ok_w;
    mb_ok_y = mb_h - mb_btn_h - 4;
}

static void msgbox_wrap(const char *text)
{
    char buf[512];
    char *word, *save;
    char cur[160];
    int maxw;

    mb_w = MB_W;
    if (mb_w > sc.sw - 40)
        mb_w = sc.sw - 40;
    maxw = mb_w - 2 * MB_PAD;
    mb_line_count = 0;
    cur[0] = '\0';

    str_copy(buf, sizeof(buf), text ? text : "");
    for (word = strtok_r(buf, " \t\n", &save); word;
         word = strtok_r(NULL, " \t\n", &save)) {
        char trial[192];

        if (cur[0] == '\0') {
            str_copy(cur, sizeof(cur), word);
        } else {
            snprintf(trial, sizeof(trial), "%s %s", cur, word);
            if (text_width(trial) > maxw) {
                if (mb_line_count < 6)
                    str_copy(mb_lines[mb_line_count++], sizeof(mb_lines[0]),
                             cur);
                str_copy(cur, sizeof(cur), word);
            } else {
                str_copy(cur, sizeof(cur), trial);
            }
        }
    }
    if (cur[0] != '\0' && mb_line_count < 6)
        str_copy(mb_lines[mb_line_count++], sizeof(mb_lines[0]), cur);
    if (mb_line_count == 0)
        str_copy(mb_lines[mb_line_count++], sizeof(mb_lines[0]), " ");
}

void msgbox_show(const char *title, const char *text)
{
    if (mb_open)
        msgbox_hide();

    str_copy(mb_title, sizeof(mb_title), title ? title : "SCDE");
    mb_w = MB_W;
    if (mb_w > sc.sw - 40)
        mb_w = sc.sw - 40;
    msgbox_wrap(text);
    msgbox_layout();

    if (mb_win == None) {
        XSetWindowAttributes wa;

        wa.override_redirect = True;
        wa.background_pixel = cfg.col_menu_bg;
        wa.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask;
        mb_win = XCreateWindow(sc.dpy, sc.root, mb_x, mb_y, (unsigned)mb_w,
                               (unsigned)mb_h, 1, CopyFromParent, InputOutput,
                               CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask,
                               &wa);
        XStoreName(sc.dpy, mb_win, "scde-msgbox");
    } else {
        XMoveResizeWindow(sc.dpy, mb_win, mb_x, mb_y, (unsigned)mb_w,
                          (unsigned)mb_h);
    }

    mb_hover_ok = 1;
    XMapRaised(sc.dpy, mb_win);
    mb_open = 1;
    XGrabKeyboard(sc.dpy, mb_win, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);
    XFlush(sc.dpy);
    log_msg("notice: %s", mb_title);
}

void msgbox_hide(void)
{
    if (!mb_open)
        return;
    mb_open = 0;
    XUngrabKeyboard(sc.dpy, CurrentTime);
    if (mb_win != None)
        XUnmapWindow(sc.dpy, mb_win);
    XFlush(sc.dpy);
}

static void msgbox_draw(void)
{
    int i, bl;
    int title_h = sc.font_height + 12;

    if (!mb_open || mb_win == None)
        return;

    XSetForeground(sc.dpy, sc.gc, cfg.col_menu_bg);
    XFillRectangle(sc.dpy, mb_win, sc.gc, 0, 0, (unsigned)mb_w,
                   (unsigned)mb_h);

    /* title strip */
    XSetForeground(sc.dpy, sc.gc, cfg.col_title_bg);
    XFillRectangle(sc.dpy, mb_win, sc.gc, 0, 0, (unsigned)mb_w,
                   (unsigned)title_h);
    bl = (title_h + sc.font_ascent - sc.font_descent) / 2;
    draw_text_clip(mb_win, MB_PAD, mb_w - MB_PAD, bl, cfg.col_title_fg,
                   mb_title);
    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XFillRectangle(sc.dpy, mb_win, sc.gc, 0, title_h - 2, (unsigned)mb_w, 2);

    /* body */
    for (i = 0; i < mb_line_count; i++) {
        int y = title_h + MB_PAD + i * sc.font_height;

        if (y + sc.font_height > mb_ok_y - 4)
            break;
        draw_text(mb_win, MB_PAD, y + sc.font_ascent + 2, cfg.col_menu_fg,
                  mb_lines[i]);
    }

    /* OK button */
    {
        unsigned long bg = mb_hover_ok ? cfg.col_btn_hover
                                       : cfg.col_btn_inactive;
        int bly = mb_ok_y + (mb_ok_h + sc.font_ascent - sc.font_descent) / 2;
        int tx = mb_ok_x + (mb_ok_w - text_width("OK")) / 2;

        XSetForeground(sc.dpy, sc.gc, bg);
        XFillRectangle(sc.dpy, mb_win, sc.gc, mb_ok_x, mb_ok_y,
                       (unsigned)mb_ok_w, (unsigned)mb_ok_h);
        XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
        XDrawRectangle(sc.dpy, mb_win, sc.gc, mb_ok_x, mb_ok_y,
                       (unsigned)(mb_ok_w - 1), (unsigned)(mb_ok_h - 1));
        draw_text(mb_win, tx, bly, cfg.col_title_fg, "OK");
    }

    XSetForeground(sc.dpy, sc.gc, cfg.col_border_focus);
    XDrawRectangle(sc.dpy, mb_win, sc.gc, 0, 0, (unsigned)(mb_w - 1),
                   (unsigned)(mb_h - 1));
}

int msgbox_expose(XExposeEvent *ev)
{
    if (!mb_open || ev->window != mb_win)
        return 0;
    if (ev->count == 0)
        msgbox_draw();
    return 1;
}

int msgbox_button(XButtonEvent *ev)
{
    int in_ok;

    if (!mb_open)
        return 0;
    if (ev->window != mb_win) {         /* click outside dismisses */
        if (ev->type == ButtonPress)
            msgbox_hide();
        return 1;
    }
    if (ev->type != ButtonPress)
        return 1;
    in_ok = ev->x >= mb_ok_x && ev->x < mb_ok_x + mb_ok_w &&
            ev->y >= mb_ok_y && ev->y < mb_ok_y + mb_ok_h;
    if (in_ok || ev->button == Button1)
        msgbox_hide();
    return 1;
}

int msgbox_motion(XMotionEvent *ev)
{
    int h;

    if (!mb_open || ev->window != mb_win)
        return 0;
    h = ev->x >= mb_ok_x && ev->x < mb_ok_x + mb_ok_w &&
        ev->y >= mb_ok_y && ev->y < mb_ok_y + mb_ok_h;
    if (h != mb_hover_ok) {
        mb_hover_ok = h;
        msgbox_draw();
    }
    return 1;
}

int msgbox_key(XKeyEvent *ev)
{
    KeySym sym;

    if (!mb_open)
        return 0;
    sym = XLookupKeysym(ev, 0);
    if (sym == XK_Escape || sym == XK_Return || sym == XK_KP_Enter ||
        sym == XK_space)
        msgbox_hide();
    return 1;
}
