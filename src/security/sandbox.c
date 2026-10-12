#define _GNU_SOURCE              /* unshare(), CLONE_NEW* */
/*
 * SCDE Security Layer - runtime feature probe and process sandboxing.
 *
 * Honest-by-design: every feature is *verified at runtime* with a forked
 * probe (seccomp must actually kill/deny, mount-ns hiding must actually
 * hide).  A header or a kernel version string is never trusted.  When a
 * feature is unavailable the callers fail closed - no fake sandbox claims.
 *
 * Tiers:
 *   RESTRICT  no_new_privs + rlimits + defensive seccomp (net/spawn still
 *             allowed) - hardening for unknown-trust targets.
 *   FULL      as RESTRICT, plus seccomp network/spawn denial and, when the
 *             fs probe passes, mount-namespace hiding of sensitive paths
 *             ($HOME, /etc, /root, /boot, /home) via tmpfs.
 *
 * Designed for Termux/PRoot: no systemd, no cgroups, no root, no landlock
 * assumption.  Only mechanisms that verify are reported AVAILABLE.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>

#include "security_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>

/*
 * <linux/filter.h> defines BPF_STMT/BPF_JUMP as brace initializers (array
 * initialisation only); these expression forms work inside emit().
 */
#define SCDE_BPF_STMT(code, k) \
    ((struct sock_filter){ (unsigned short)(code), 0, 0, (unsigned)(k) })
#define SCDE_BPF_JUMP(code, jt, jf, k)                                       \
    ((struct sock_filter){ (unsigned short)(code), (unsigned char)(jt),      \
                           (unsigned char)(jf), (unsigned)(k) })
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif
#ifndef PR_GET_NO_NEW_PRIVS
#define PR_GET_NO_NEW_PRIVS 39
#endif
#ifndef PR_SET_SECCOMP
#define PR_SET_SECCOMP 22
#endif
#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif

#if defined(__x86_64__)
#define SCDE_AUDIT_ARCH_ AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SCDE_AUDIT_ARCH_ AUDIT_ARCH_AARCH64
#elif defined(__arm__)
#define SCDE_AUDIT_ARCH_ AUDIT_ARCH_ARM
#elif defined(__i386__)
#define SCDE_AUDIT_ARCH_ AUDIT_ARCH_I386
#else
#define SCDE_AUDIT_ARCH_ 0
#endif

/* filter instruction cap: deny lists are small, headroom for arg checks */
#define MAX_INSNS 128

static scde_security_features_t feat;
static int feat_done;

/* ------------------------------------------------------------ helpers -- */

const char *scde_feature_state_name(scde_feature_state_t state)
{
    switch (state) {
    case SCDE_FEATURE_AVAILABLE:
        return "AVAILABLE";
    case SCDE_FEATURE_LIMITED:
        return "LIMITED";
    default:
        return "UNAVAILABLE";
    }
}

const char *scde_sandbox_tier_name(scde_sandbox_tier_t tier)
{
    switch (tier) {
    case SCDE_SANDBOX_FULL:
        return "full";
    case SCDE_SANDBOX_RESTRICT:
        return "restrict";
    default:
        return "none";
    }
}

static int run_child(int (*body)(void))
{
    pid_t pid;
    int st;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(body());
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    if (!WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

/* --------------------------------------------------------------- probe -- */

static int probe_nnp_body(void)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return 1;
    if (prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
        return 2;
    return 0;
}

static int probe_rlimit_body(void)
{
    struct rlimit rl;
    struct rlimit got;

    rl.rlim_cur = 128;
    rl.rlim_max = 128;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 1;
    if (getrlimit(RLIMIT_NOFILE, &got) != 0)
        return 2;
    if (got.rlim_cur == 0 || got.rlim_cur > 128)
        return 3;
    return 0;
}

/* installs a filter that must deny getppid with EPERM, allow everything else */
static int seccomp_install(const struct sock_filter *filt, unsigned int len)
{
    struct sock_fprog prog;

    prog.len = len;
    prog.filter = (struct sock_filter *)filt;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    return (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

static int probe_seccomp_body(void)
{
    struct sock_filter filt[] = {
        SCDE_BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, arch)),
        SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SCDE_AUDIT_ARCH_, 1, 0),
        SCDE_BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        SCDE_BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, nr)),
        SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getppid, 0, 1),
        SCDE_BPF_STMT(BPF_RET | BPF_K,
                 SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
        SCDE_BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    long r;
    long p;

    if (SCDE_AUDIT_ARCH_ == 0)
        return 1;
    if (seccomp_install(filt, sizeof(filt) / sizeof(filt[0])) != 0)
        return 2;
    /* allow path must work */
    r = getpid();
    if (r <= 0)
        return 3;
    /* deny path must be enforced by the kernel, not just installed */
    errno = 0;
    p = getppid();
    if (p != -1 || errno != EPERM)
        return 4;
    return 0;
}

static void write_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY);

    if (fd < 0)
        return;
    if (write(fd, s, strlen(s)) < 0) { /* best effort */ }
    close(fd);
}

/* full fs-isolation sequence: userns + map + private mounts + tmpfs hide */
static int probe_fs_body(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];
    int fd;

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
        return 1;
    write_str("/proc/self/setgroups", "deny");
    snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)uid);
    write_str("/proc/self/uid_map", buf);
    snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)gid);
    write_str("/proc/self/gid_map", buf);
    if (getuid() != 0)
        return 2;
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return 3;
    if (mount("tmpfs", "/etc", "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") != 0)
        return 4;
    /* hidden now: must fail */
    fd = open("/etc/hosts", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return 5;
    }
    /* untouched paths must keep working */
    fd = open("/bin/sh", O_RDONLY);
    if (fd < 0)
        fd = open("/usr/bin/env", O_RDONLY);
    if (fd < 0)
        return 6;
    close(fd);
    return 0;
}

static int proot_detected(void)
{
    char line[256];
    FILE *f;

    if (getenv("PROOT") || getenv("PROOT_L2S_DIR") || getenv("PROOT_TMP_DIR"))
        return 1;
    f = fopen("/proc/self/status", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "TracerPid:", 10) == 0) {
                long t = strtol(line + 10, NULL, 10);

                fclose(f);
                return t > 0;
            }
        }
        fclose(f);
    }
    return 0;
}

static void probe_features(void)
{
    /* run_child() depends on waitpid().  The WM runs with SIGCHLD set to
     * SIG_IGN (fire-and-forget app spawning), under which forked probe
     * children are auto-reaped and waitpid() always fails - every probe
     * would wrongly report UNAVAILABLE and fail-closed spawning would
     * break.  Probes run once and SCDE is single-threaded, so borrow
     * SIG_DFL for the probe window; restoring SIG_IGN afterwards also
     * discards any zombie the window may have collected. */
    void (*old_chld)(int) = SIG_DFL;
    int borrowed = 0;

    if (feat_done)
        return;
    old_chld = signal(SIGCHLD, SIG_DFL);
    borrowed = (old_chld != SIG_ERR);
    memset(&feat, 0, sizeof(feat));

    feat.proot = proot_detected();

    feat.no_new_privs = run_child(probe_nnp_body) == 0
                            ? SCDE_FEATURE_AVAILABLE
                            : SCDE_FEATURE_UNAVAILABLE;
    feat.rlimit = run_child(probe_rlimit_body) == 0
                      ? SCDE_FEATURE_AVAILABLE
                      : SCDE_FEATURE_UNAVAILABLE;
    feat.seccomp = run_child(probe_seccomp_body) == 0
                       ? SCDE_FEATURE_AVAILABLE
                       : SCDE_FEATURE_UNAVAILABLE;
    feat.fs = run_child(probe_fs_body) == 0
                  ? SCDE_FEATURE_AVAILABLE
                  : SCDE_FEATURE_UNAVAILABLE;

    /* honest combination labels */
    if (feat.no_new_privs == SCDE_FEATURE_AVAILABLE &&
        feat.rlimit == SCDE_FEATURE_AVAILABLE &&
        feat.seccomp == SCDE_FEATURE_AVAILABLE)
        feat.kernel_enforcement = SCDE_FEATURE_AVAILABLE;
    else if (feat.no_new_privs == SCDE_FEATURE_AVAILABLE &&
             feat.rlimit == SCDE_FEATURE_AVAILABLE)
        feat.kernel_enforcement = SCDE_FEATURE_LIMITED;
    else
        feat.kernel_enforcement = SCDE_FEATURE_UNAVAILABLE;

    if (feat.seccomp == SCDE_FEATURE_UNAVAILABLE)
        feat.sandbox = SCDE_FEATURE_UNAVAILABLE;
    else if (feat.fs == SCDE_FEATURE_AVAILABLE)
        feat.sandbox = SCDE_FEATURE_AVAILABLE;
    else
        feat.sandbox = SCDE_FEATURE_LIMITED;

    feat.scanner_active =
        scde_security_get_scan_mode() != SCDE_SCAN_OFF;
    feat.policy_active = 1;             /* policy gate is always compiled in */
    feat_done = 1;
    if (borrowed)
        signal(SIGCHLD, old_chld);
}

int scde_security_features(scde_security_features_t *out)
{
    if (!out)
        return SCDE_ERR_ARG;
    scde__ensure();
    probe_features();
    *out = feat;
    return SCDE_OK;
}

/* --------------------------------------------------------- seccomp BPF -- */

typedef struct {
    struct sock_filter insns[MAX_INSNS];
    unsigned int n;
} prog_t;

static void emit(prog_t *p, struct sock_filter in)
{
    if (p->n < MAX_INSNS)
        p->insns[p->n++] = in;
}

static void emit_arch_check(prog_t *p)
{
    emit(p, SCDE_BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     (unsigned int)offsetof(struct seccomp_data, arch)));
    emit(p, SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SCDE_AUDIT_ARCH_, 1, 0));
    emit(p, SCDE_BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    emit(p, SCDE_BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     (unsigned int)offsetof(struct seccomp_data, nr)));
}

/* JEQ nr -> ERRNO(EPERM): match executes the RET, non-match skips it. */
static void emit_deny_nr(prog_t *p, int nr)
{
    emit(p, SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)nr, 0, 1));
    emit(p, SCDE_BPF_STMT(BPF_RET | BPF_K,
                     SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
}

/* socket()/socketpair(): only AF_UNIX passes when net is denied.
 * On entry A = syscall nr.  Layout (verified offsets):
 *   s0: JEQ socket     -> s3 (jt 2), else s1
 *   s1: JEQ socketpair -> s3 (jt 1), else s2
 *   s2: JA 3           -> s6
 *   s3: LD args[0]
 *   s4: JEQ AF_UNIX    -> s6 (jt 1), else s5
 *   s5: RET ERRNO(EPERM)
 *   s6: continue chain
 */
static void emit_deny_nonunix_socket(prog_t *p)
{
    emit(p, SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 2, 0));
    emit(p, SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 1, 0));
    emit(p, SCDE_BPF_STMT(BPF_JMP | BPF_JA | BPF_K, 3));
    emit(p, SCDE_BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                     (unsigned int)offsetof(struct seccomp_data, args[0])));
    emit(p, SCDE_BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 1, 0));
    emit(p, SCDE_BPF_STMT(BPF_RET | BPF_K,
                     SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
}

static void emit_admin_denies(prog_t *p)
{
#ifdef __NR_mount
    emit_deny_nr(p, __NR_mount);
#endif
#ifdef __NR_umount2
    emit_deny_nr(p, __NR_umount2);
#endif
#ifdef __NR_pivot_root
    emit_deny_nr(p, __NR_pivot_root);
#endif
#ifdef __NR_chroot
    emit_deny_nr(p, __NR_chroot);
#endif
#ifdef __NR_unshare
    emit_deny_nr(p, __NR_unshare);
#endif
#ifdef __NR_setns
    emit_deny_nr(p, __NR_setns);
#endif
#ifdef __NR_fsopen
    emit_deny_nr(p, __NR_fsopen);
#endif
#ifdef __NR_fsconfig
    emit_deny_nr(p, __NR_fsconfig);
#endif
#ifdef __NR_fsmount
    emit_deny_nr(p, __NR_fsmount);
#endif
#ifdef __NR_move_mount
    emit_deny_nr(p, __NR_move_mount);
#endif
#ifdef __NR_open_tree
    emit_deny_nr(p, __NR_open_tree);
#endif
#ifdef __NR_mount_setattr
    emit_deny_nr(p, __NR_mount_setattr);
#endif
#ifdef __NR_init_module
    emit_deny_nr(p, __NR_init_module);
#endif
#ifdef __NR_finit_module
    emit_deny_nr(p, __NR_finit_module);
#endif
#ifdef __NR_delete_module
    emit_deny_nr(p, __NR_delete_module);
#endif
#ifdef __NR_kexec_load
    emit_deny_nr(p, __NR_kexec_load);
#endif
#ifdef __NR_kexec_file_load
    emit_deny_nr(p, __NR_kexec_file_load);
#endif
#ifdef __NR_reboot
    emit_deny_nr(p, __NR_reboot);
#endif
#ifdef __NR_swapon
    emit_deny_nr(p, __NR_swapon);
#endif
#ifdef __NR_swapoff
    emit_deny_nr(p, __NR_swapoff);
#endif
#ifdef __NR_settimeofday
    emit_deny_nr(p, __NR_settimeofday);
#endif
#ifdef __NR_clock_settime
    emit_deny_nr(p, __NR_clock_settime);
#endif
#ifdef __NR_bpf
    emit_deny_nr(p, __NR_bpf);
#endif
#ifdef __NR_perf_event_open
    emit_deny_nr(p, __NR_perf_event_open);
#endif
#ifdef __NR_ptrace
    emit_deny_nr(p, __NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
    emit_deny_nr(p, __NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    emit_deny_nr(p, __NR_process_vm_writev);
#endif
#ifdef __NR_userfaultfd
    emit_deny_nr(p, __NR_userfaultfd);
#endif
#ifdef __NR_acct
    emit_deny_nr(p, __NR_acct);
#endif
#ifdef __NR_syslog
    emit_deny_nr(p, __NR_syslog);
#endif
}

static int install_filter(int full)
{
    prog_t p;

    memset(&p, 0, sizeof(p));
    if (SCDE_AUDIT_ARCH_ == 0)
        return -1;
    emit_arch_check(&p);

    if (full) {
        /* no child processes */
#ifdef __NR_clone
        emit_deny_nr(&p, __NR_clone);
#endif
#ifdef __NR_vfork
        emit_deny_nr(&p, __NR_vfork);
#endif
#ifdef __NR_fork
        emit_deny_nr(&p, __NR_fork);
#endif
#ifdef __NR_clone3
        emit_deny_nr(&p, __NR_clone3);
#endif
        /* no keyring smuggling */
#ifdef __NR_add_key
        emit_deny_nr(&p, __NR_add_key);
#endif
#ifdef __NR_request_key
        emit_deny_nr(&p, __NR_request_key);
#endif
#ifdef __NR_keyctl
        emit_deny_nr(&p, __NR_keyctl);
#endif
        /* io_uring bypasses classic seccomp syscall filtering */
#ifdef __NR_io_uring_setup
        emit_deny_nr(&p, __NR_io_uring_setup);
#endif
#ifdef __NR_io_uring_enter
        emit_deny_nr(&p, __NR_io_uring_enter);
#endif
        emit_deny_nonunix_socket(&p);
    }

    emit_admin_denies(&p);

    emit(&p, SCDE_BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    if (p.n > MAX_INSNS)
        return -1;
    return seccomp_install(p.insns, p.n);
}

/* --------------------------------------------------------- fs hiding -- */

static int path_is_within(const char *path, const char *dir)
{
    size_t n;

    if (!path || !dir || !*dir)
        return 0;
    n = strlen(dir);
    if (strncmp(path, dir, n) != 0)
        return 0;
    return path[n] == '\0' || path[n] == '/' ||
           (n > 0 && dir[n - 1] == '/');
}

static int mount_tmpfs_over(const char *dir)
{
    struct stat st;

    if (lstat(dir, &st) != 0)
        return 0;                       /* nothing to hide */
    if (mount("tmpfs", dir, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755") != 0)
        return -1;
    return 0;
}

static char staged_xauth[SCDE_PATH_MAX];
static char xauth_dest[SCDE_PATH_MAX];

/*
 * X11 cookie preservation: copy (not bind) the authority file before the
 * HOME tmpfs hides it, then recreate it inside the hidden HOME.  Copying
 * works under PRoot where file bind-mounts are unreliable.
 */
#define XAUTH_STAGED_MAX (64 * 1024)

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    char buf[4096];
    int in;
    int out;
    ssize_t got;
    size_t total = 0;

    /* O_NOFOLLOW on both ends: never read through or write through a link
       an attacker may have planted */
    in = open(src, O_RDONLY | O_NOFOLLOW);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((got = read(in, buf, sizeof(buf))) > 0) {
        total += (size_t)got;
        if (total > XAUTH_STAGED_MAX) {
            close(in);
            close(out);
            unlink(dst);
            return -1;
        }
        if (write(out, buf, (size_t)got) != got) {
            close(in);
            close(out);
            unlink(dst);
            return -1;
        }
    }
    close(in);
    close(out);
    if (got < 0) {
        unlink(dst);
        return -1;
    }
    return 0;
}

static void stage_xauth(void)
{
    const char *xa = getenv("XAUTHORITY");
    struct stat st;
    char tmpl[64];
    int tmpfd;

    staged_xauth[0] = '\0';
    xauth_dest[0] = '\0';
    if (!xa || xa[0] != '/' || stat(xa, &st) != 0 || st.st_size < 0 ||
        st.st_size > XAUTH_STAGED_MAX)
        return;
    snprintf(xauth_dest, sizeof(xauth_dest), "%.*s",
             (int)(sizeof(xauth_dest) - 1), xa);
    /* mkstemp creates the staging file atomically (O_EXCL, 0600): a
       pre-planted symlink or regular file can never hijack the copy */
    snprintf(tmpl, sizeof(tmpl), "/tmp/.scde-xauth-XXXXXX");
    tmpfd = mkstemp(tmpl);
    if (tmpfd < 0)
        return;
    close(tmpfd);
    if (copy_file(xa, tmpl, 0600) != 0) {
        unlink(tmpl);
        return;
    }
    snprintf(staged_xauth, sizeof(staged_xauth), "%.*s",
             (int)(sizeof(staged_xauth) - 1), tmpl);
}

static void make_parents(const char *path)
{
    char buf[SCDE_PATH_MAX];
    char *p;

    snprintf(buf, sizeof(buf), "%.*s", (int)(sizeof(buf) - 1), path);
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0700);          /* best effort inside the tmpfs */
            *p = '/';
        }
    }
}

static void finish_xauth(void)
{
    if (!staged_xauth[0])
        return;
    make_parents(xauth_dest);
    (void)copy_file(staged_xauth, xauth_dest, 0600);
    unlink(staged_xauth);
    staged_xauth[0] = '\0';
}

static int hide_sensitive_paths(void)
{
    static const char *const dirs[] = { "/etc", "/root", "/boot", "/home",
                                        NULL };
    const char *home = getenv("HOME");
    int i;
    int hidden_any = 0;

    stage_xauth();                      /* before HOME is covered */
    for (i = 0; dirs[i]; i++) {
        if (mount_tmpfs_over(dirs[i]) != 0)
            return -1;
        hidden_any = 1;
    }
    if (home && home[0] == '/' && !path_is_within(home, "/home") &&
        !path_is_within(home, "/root")) {
        if (mount_tmpfs_over(home) != 0)
            return -1;
        hidden_any = 1;
    }
    finish_xauth();                     /* recreate inside the hidden tree */
    return hidden_any ? 0 : -1;
}

/* ------------------------------------------------------ sandbox apply -- */

static void apply_rlimits(int full)
{
    struct rlimit rl;

    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);

    rl.rlim_cur = rl.rlim_max = full ? 128 : 256;
    setrlimit(RLIMIT_NOFILE, &rl);

    rl.rlim_cur = rl.rlim_max = full ? 64 : 512;
#ifdef RLIMIT_NPROC
    setrlimit(RLIMIT_NPROC, &rl);
#else
    (void)0;
#endif

    rl.rlim_cur = rl.rlim_max = full ? (rlim_t)(512 * 1024 * 1024)
                                     : (rlim_t)(1024 * 1024 * 1024);
    setrlimit(RLIMIT_FSIZE, &rl);

    /* RAM + CPU: kernel-enforced where the platform allows.
     * Best effort here (the dedicated resource module is authoritative
     * and fail-closed); a refusal here must not silently widen limits. */
    {
        struct rlimit current;

        rl.rlim_cur = rl.rlim_max =
            full ? (rlim_t)(512 * 1024 * 1024) : (rlim_t)(1024UL * 1024UL * 1024UL);
#ifdef RLIMIT_AS
        if (getrlimit(RLIMIT_AS, &current) == 0) {
            if (current.rlim_cur == RLIM_INFINITY ||
                current.rlim_cur > rl.rlim_cur)
                setrlimit(RLIMIT_AS, &rl);
        } else {
            setrlimit(RLIMIT_AS, &rl);
        }
#endif
        rl.rlim_cur = rl.rlim_max = full ? (rlim_t)30 : (rlim_t)120;
        if (getrlimit(RLIMIT_CPU, &current) == 0) {
            if (current.rlim_cur == RLIM_INFINITY ||
                current.rlim_cur > rl.rlim_cur)
                setrlimit(RLIMIT_CPU, &rl);
        } else {
            setrlimit(RLIMIT_CPU, &rl);
        }
    }
}

int scde_sandbox_apply(scde_sandbox_tier_t tier)
{
    scde_security_features_t f;

    if (tier == SCDE_SANDBOX_NONE)
        return SCDE_OK;
    if (tier != SCDE_SANDBOX_RESTRICT && tier != SCDE_SANDBOX_FULL)
        return SCDE_ERR_ARG;

    scde_security_features(&f);

    /* no_new_privs is the baseline for every restricted tier */
    if (f.no_new_privs != SCDE_FEATURE_AVAILABLE)
        return SCDE_ERR;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return SCDE_ERR;
    apply_rlimits(tier == SCDE_SANDBOX_FULL);

    if (tier == SCDE_SANDBOX_FULL) {
        /* seccomp is REQUIRED for the sandbox tier: fail closed */
        if (f.seccomp != SCDE_FEATURE_AVAILABLE)
            return SCDE_ERR;
        /* filesystem hiding first: seccomp then blocks ns churn */
        if (f.fs == SCDE_FEATURE_AVAILABLE) {
            if (hide_sensitive_paths() != 0)
                return SCDE_ERR;
        }
        if (install_filter(1) != 0)
            return SCDE_ERR;
        return SCDE_OK;
    }

    /* RESTRICT: seccomp is defence-in-depth; nnp+rlimits already applied */
    if (f.seccomp == SCDE_FEATURE_AVAILABLE) {
        if (install_filter(0) != 0)
            return SCDE_ERR;
    }
    return SCDE_OK;
}
