/*
 * SCDE - utility helpers
 */
#include "scde.h"

#include <scde/security.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

/* internal: subject of the most recent security exec gate (core.c) */
const char *scde__last_exec_subject(void);

void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[scde] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[scde] fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/*
 * Run a command through /bin/sh in its own session.  The X connection fd and
 * the instance lock fd are closed in the child so it never inherits them.
 *
 * Every command first passes the SCDE security policy gate (resolve ->
 * scan -> trust -> decide, fail-closed).  DENY is refused here and audited.
 * RESTRICT/SANDBOX children apply their restrictions before exec and
 * acknowledge them over a CLOEXEC pipe; without a verified ack the parent
 * never lets the run continue silently - denied runs are audited too.
 */
void spawn(const char *cmd)
{
    pid_t pid;
    scde_decision_t dec = SCDE_DECIDE_DENY;
    int pfd[2] = { -1, -1 };
    const char *subject;

    if (!cmd || !*cmd)
        return;

    /* policy gate: logs DENY / POLICY_FALLBACK / FEATURE_UNAVAILABLE */
    if (scde_security_gate_exec(cmd, &dec) != SCDE_OK)
        dec = SCDE_DECIDE_DENY;
    subject = scde__last_exec_subject();
    if (dec == SCDE_DECIDE_DENY) {
        log_msg("spawn: blocked by security policy (%s)", subject);
        return;
    }

    if (dec != SCDE_DECIDE_ALLOW && pipe(pfd) != 0) {
        /* fail-closed: no verified channel for restrictions -> no run */
        scde_audit_log(SCDE_AUDIT_SANDBOX_FAILED, subject, 80,
                       "cannot establish restriction channel");
        log_msg("spawn: cannot restrict (%s)", subject);
        return;
    }

    pid = fork();
    if (pid < 0) {
        if (pfd[0] >= 0) {
            close(pfd[0]);
            close(pfd[1]);
        }
        log_msg("fork failed");
        return;
    }
    if (pid > 0) {
        char ack = 0;
        ssize_t n;

        if (pfd[1] >= 0)
            close(pfd[1]);
        if (pfd[0] < 0)
            return;
        n = read(pfd[0], &ack, 1);
        close(pfd[0]);
        if (n == 1 && ack == 'O') {
            if (dec == SCDE_DECIDE_SANDBOX)
                scde_audit_log(SCDE_AUDIT_SANDBOX_CREATED, subject, 0,
                               "sandbox verified");
            else
                scde_audit_log(SCDE_AUDIT_PROCESS_RESTRICTED, subject, 0,
                               "restrictions verified");
        } else {
            scde_audit_log(SCDE_AUDIT_SANDBOX_FAILED, subject, 80,
                           "child failed to apply restrictions");
            log_msg("spawn: restriction failed (%s)", subject);
        }
        return;
    }

    /* child */
    setsid();
    if (sc.dpy)
        close(ConnectionNumber(sc.dpy));
    if (sc.lock_fd >= 0)
        close(sc.lock_fd);
    if (pfd[0] >= 0)
        close(pfd[0]);

    if (dec != SCDE_DECIDE_ALLOW) {
        scde_sandbox_tier_t tier =
            (dec == SCDE_DECIDE_SANDBOX) ? SCDE_SANDBOX_FULL
                                         : SCDE_SANDBOX_RESTRICT;
        char ack = 'F';

        if (scde_sandbox_apply(tier) == SCDE_OK)
            ack = 'O';
        if (pfd[1] >= 0) {
            (void)!write(pfd[1], &ack, 1);
            close(pfd[1]);
        }
        if (ack != 'O')
            _exit(126);                 /* fail-closed: never run loose */
    }

    {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
    }
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

/* quote s for /bin/sh into out: always single-quoted, embedded ' escaped */
void shell_quote(char *out, size_t size, const char *s)
{
    size_t o = 0;

    if (size == 0)
        return;
    if (!s)
        s = "";
    if (o + 1 < size)
        out[o++] = '\'';
    for (; *s && o + 3 < size; s++) {
        if (*s == '\'') {
            if (o + 4 >= size)
                break;
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *s;
        }
    }
    if (o + 1 < size)
        out[o++] = '\'';
    out[o] = '\0';
}

unsigned long parse_color(const char *str, unsigned long fallback)
{
    XColor color, exact;

    if (!str || !*str || !sc.dpy)
        return fallback;

    if (XAllocNamedColor(sc.dpy, sc.cmap, str, &color, &exact))
        return color.pixel;
    if (XParseColor(sc.dpy, sc.cmap, str, &color) &&
        XAllocColor(sc.dpy, sc.cmap, &color))
        return color.pixel;

    log_msg("cannot parse colour '%s', using fallback", str);
    return fallback;
}

/* parse "#rrggbb" / "rgb:rr/gg/bb" into 0-255 components; 0 on failure */
int color_rgb(const char *str, int *r, int *g, int *b)
{
    unsigned int rv = 0, gv = 0, bv = 0;
    int n;

    if (!str)
        return 0;
    if (str[0] == '#' && strlen(str) >= 7) {
        n = sscanf(str + 1, "%2x%2x%2x", &rv, &gv, &bv);
        if (n != 3)
            return 0;
    } else if (strncmp(str, "rgb:", 4) == 0) {
        n = sscanf(str + 4, "%x/%x/%x", &rv, &gv, &bv);
        if (n != 3)
            return 0;
        rv &= 0xff;
        gv &= 0xff;
        bv &= 0xff;
    } else {
        XColor xc;

        if (!sc.dpy || !XParseColor(sc.dpy, sc.cmap, str, &xc))
            return 0;
        rv = (unsigned)(xc.red >> 8);
        gv = (unsigned)(xc.green >> 8);
        bv = (unsigned)(xc.blue >> 8);
    }
    *r = (int)rv;
    *g = (int)gv;
    *b = (int)bv;
    return 1;
}

char *str_trim(char *s)
{
    char *end;
    size_t len;

    if (!s)
        return s;
    len = strlen(s);
    while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                   s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

void str_copy(char *dst, size_t size, const char *src)
{
    size_t len;

    if (size == 0)
        return;
    len = src ? strlen(src) : 0;
    if (len >= size)
        len = size - 1;
    if (len)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

int text_width(const char *s)
{
    if (!s)
        return 0;
    if (sc.font)
        return XTextWidth(sc.font, s, (int)strlen(s));
    return (int)strlen(s) * 7;
}

/* truncate src so it fits into max_w pixels, appending "..." when needed */
void fit_text(char *dst, size_t dst_size, const char *src, int max_w)
{
    size_t len;

    if (dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    str_copy(dst, dst_size, src);
    if (text_width(dst) <= max_w)
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
        if (text_width(dst) <= max_w)
            return;
        if (len < 4)
            break;
    }
    dst[0] = '\0';
}

void draw_text(Window w, int x, int baseline_y, unsigned long color, const char *s)
{
    int len;

    if (!s)
        return;
    len = (int)strlen(s);
    if (len == 0)
        return;
    XSetForeground(sc.dpy, sc.gc, color);
    if (sc.font)
        XSetFont(sc.dpy, sc.gc, sc.font->fid);
    XDrawString(sc.dpy, w, sc.gc, x, baseline_y, s, len);
}

/* draw text, clipped so it never paints past max_x */
void draw_text_clip(Window w, int x, int max_x, int baseline_y,
                    unsigned long color, const char *s)
{
    char buf[NAME_MAX_LEN];
    int avail = max_x - x;

    if (avail <= 4 || !s)
        return;
    fit_text(buf, sizeof(buf), s, avail);
    draw_text(w, x, baseline_y, color, buf);
}

/*
 * Mini "application window" glyph: a filled square with a highlighted title
 * strip on top.  Used for the title bar icon and the task bar buttons so a
 * window always shows an icon without needing X icon hints from the client.
 */
void draw_win_glyph(Window w, int x, int y, int size, unsigned long fg,
                    unsigned long bg)
{
    if (size < 6)
        return;
    XSetForeground(sc.dpy, sc.gc, bg);
    XFillRectangle(sc.dpy, w, sc.gc, x, y, (unsigned)size, (unsigned)size);
    XSetForeground(sc.dpy, sc.gc, fg);
    XFillRectangle(sc.dpy, w, sc.gc, x + 1, y + 1, (unsigned)(size - 2), 3);
    XDrawRectangle(sc.dpy, w, sc.gc, x, y, (unsigned)(size - 1),
                   (unsigned)(size - 1));
}
