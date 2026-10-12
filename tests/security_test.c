/*
 * SCDE security boundary tests: decision matrix, capability gate, plugin
 * trust gate, exec gate (fail-closed), sandbox tiers (seccomp/fs hiding),
 * resource limits, bait/mirage decoys, spawn end-to-end, audit consistency.
 * Non-destructive: everything lives in a mkdtemp sandbox under /tmp,
 * loopback network only, no real targets, dummy decoys only.
 *
 * usage: tests/security_test <example.so>
 */
#include "../src/scde.h"                 /* Scde sc (spawn linkage) */

#include <scde/app.h>
#include <scde/common.h>
#include <scde/security.h>
#include <scde/security_bait.h>
#include <scde/security_mirage.h>
#include <scde/security_resource.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, fails;

#define CHECK(cond, what)                                                   \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            fails++;                                                        \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, what);   \
        }                                                                   \
    } while (0)

#define CHECK_INT(got, want, what)                                          \
    do {                                                                    \
        int g_ = (int)(got), w_ = (int)(want);                              \
        checks++;                                                           \
        if (g_ != w_) {                                                     \
            fails++;                                                        \
            fprintf(stderr, "FAIL %s:%d %s (got %d want %d)\n",             \
                    __FILE__, __LINE__, what, g_, w_);                      \
        }                                                                   \
    } while (0)

/* required by src/util.c (spawn) */
Scde sc;

static char root[256];             /* mkdtemp sandbox */
static char payload[300];          /* known-bad script */
static char marker_allow[300];
static char marker_sb[300];

static void res_init(scde_scan_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->status = SCDE_THREAT_UNKNOWN;
    r->trust = SCDE_TRUST_UNKNOWN;
    r->hash_status = SCDE_HASH_SKIPPED;
}

static int exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int poll_exists(const char *p, int ms)
{
    int i;

    for (i = 0; i < ms / 10; i++) {
        if (exists(p))
            return 1;
        sleep_ms(10);
    }
    return exists(p);
}

static void write_file(const char *path, const char *content, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd >= 0) {
        (void)!write(fd, content, strlen(content));
        close(fd);
    }
    chmod(path, mode);
}

static int nnp_status(void)       /* /proc NoNewPrivs, no extra headers */
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    int v = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, "NoNewPrivs:", 11) == 0) {
            v = atoi(line + 11);
            break;
        }
    fclose(f);
    return v;
}

static int count_event(int event)
{
    return (int)scde_audit_total(event);
}

/* run fn() in a forked child, return its exit code (or -1 fork fail) */
static int child_run(int (*fn)(void))
{
    pid_t pid = fork();
    int st = 0;

    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(fn());
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    if (!WIFEXITED(st))
        return -2;
    return WEXITSTATUS(st);
}

/* ----------------------------------------------------------- features -- */

static void test_features(void)
{
    scde_security_features_t f;

    CHECK_INT(scde_security_features(&f), SCDE_OK, "features()");
    CHECK(scde_feature_state_name(f.kernel_enforcement) != NULL,
          "kernel state name");
    CHECK(scde_feature_state_name(f.sandbox) != NULL, "sandbox state name");
    CHECK(f.policy_active, "policy gate present");
    CHECK(f.scanner_active, "scanner active (always-on)");
    if (f.sandbox == SCDE_FEATURE_AVAILABLE)
        CHECK(f.seccomp == SCDE_FEATURE_AVAILABLE,
              "available sandbox implies seccomp");
    if (f.sandbox == SCDE_FEATURE_UNAVAILABLE)
        CHECK(f.seccomp != SCDE_FEATURE_AVAILABLE,
              "no seccomp means no sandbox");
    if (f.kernel_enforcement == SCDE_FEATURE_AVAILABLE)
        CHECK(f.no_new_privs == SCDE_FEATURE_AVAILABLE &&
              f.seccomp == SCDE_FEATURE_AVAILABLE,
              "kernel enforcement = nnp + seccomp");
}

/* ------------------------------------------------------------ decide --- */

static void test_decide(void)
{
    scde_scan_result_t r;
    scde_decision_t d = SCDE_DECIDE_ALLOW;
    scde_security_features_t f;

    scde_security_features(&f);

    res_init(&r);
    r.status = SCDE_THREAT_BLOCKED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "blocked rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "blocked -> deny");

    res_init(&r);
    r.status = SCDE_THREAT_MALICIOUS;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "malicious rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "malicious -> deny");

    res_init(&r);
    r.trust = SCDE_TRUST_BLOCKED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "trust-blocked rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "trust-blocked -> deny");

    res_init(&r);
    r.status = SCDE_THREAT_UNKNOWN;
    r.trust = SCDE_TRUST_UNKNOWN;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "unknown rc");
    CHECK_INT(d, SCDE_DECIDE_RESTRICT, "unknown trust -> restrict");

    res_init(&r);
    r.trust = SCDE_TRUST_TRUSTED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "trusted rc");
    CHECK_INT(d, SCDE_DECIDE_ALLOW, "trusted -> allow");

    res_init(&r);
    r.trust = SCDE_TRUST_UNTRUSTED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "untrusted rc");
    CHECK_INT(d, f.sandbox == SCDE_FEATURE_UNAVAILABLE ? SCDE_DECIDE_DENY
                                                       : SCDE_DECIDE_SANDBOX,
              "untrusted -> sandbox (deny when unavailable)");

    res_init(&r);
    r.status = SCDE_THREAT_SUSPICIOUS;
    r.trust = SCDE_TRUST_UNTRUSTED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "suspicious rc");
    CHECK_INT(d, f.sandbox == SCDE_FEATURE_UNAVAILABLE ? SCDE_DECIDE_DENY
                                                       : SCDE_DECIDE_SANDBOX,
              "suspicious -> sandbox (deny when unavailable)");

    scde_policy_set(SCDE_POL_DENY_SUSPICIOUS, 1);
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "deny-susp rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "deny-suspicious policy -> deny");
    scde_policy_set(SCDE_POL_DENY_SUSPICIOUS, 0);

    res_init(&r);
    r.status = (int16_t)99;             /* unknown status */
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "bad-status rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "unknown status -> deny (fail-closed)");

    d = SCDE_DECIDE_ALLOW;
    CHECK_INT(scde_security_decide(NULL, &d), SCDE_ERR_ARG, "null rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "null -> fail-closed");
}

/* -------------------------------------------------------- capabilities -- */

static void test_capabilities(void)
{
    scde_security_revoke("test:alpha", 0xffffffffu);

    CHECK_INT(scde_security_grant("test:alpha",
                                  SCDE_CAP_FS_READ | SCDE_CAP_CONFIG),
              SCDE_OK, "grant read+config");
    CHECK_INT(scde_security_check("test:alpha", SCDE_CAP_FS_READ), SCDE_OK,
              "granted bit checks");
    CHECK_INT(scde_security_check("test:alpha", SCDE_CAP_CONFIG), SCDE_OK,
              "second granted bit checks");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_FS_WRITE) != SCDE_OK,
          "write not granted");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_SPAWN) != SCDE_OK,
          "spawn never implicit");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_NET) != SCDE_OK,
          "net never implicit");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_DEVICE) != SCDE_OK,
          "device never implicit");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_PLUGIN_LOAD) != SCDE_OK,
          "plugin.load never implicit");
    CHECK(scde_security_check("nobody", SCDE_CAP_FS_READ) != SCDE_OK,
          "unknown subject denied");

    CHECK_INT(scde_security_revoke("test:alpha", 0xffffffffu), SCDE_OK,
              "revoke all");
    CHECK(scde_security_check("test:alpha", SCDE_CAP_FS_READ) != SCDE_OK,
          "revoked bit denied");
}

/* --------------------------------------------------------- exec gate --- */

static void test_gate(void)
{
    scde_decision_t d = SCDE_DECIDE_ALLOW;
    scde_scan_stats_t before, after;
    int bad_before;

    CHECK_INT(scde_security_gate_exec(NULL, &d), SCDE_ERR_ARG, "gate null");
    CHECK_INT(d, SCDE_DECIDE_DENY, "gate null -> deny");
    CHECK_INT(scde_security_gate_exec("", &d), SCDE_ERR_ARG, "gate empty");

    /* resolvable normal command: never DENY (heap realpath: a stack
     * buffer under PATH_MAX trips FORTIFY's __realpath_chk) */
    {
        char *rt = realpath("/bin/true", NULL);

        CHECK(rt != NULL, "realpath /bin/true");
        free(rt);
    }
    d = SCDE_DECIDE_DENY;
    CHECK_INT(scde_security_gate_exec("/bin/true", &d), SCDE_OK, "gate true");
    CHECK(d != SCDE_DECIDE_DENY, "/bin/true not denied");

    /* unresolvable target: restricted fallback, audited */
    bad_before = count_event(SCDE_AUDIT_POLICY_FALLBACK);
    d = SCDE_DECIDE_ALLOW;
    CHECK_INT(scde_security_gate_exec("scde-no-such-binary-xyz", &d), SCDE_OK,
              "gate missing");
    CHECK_INT(d, SCDE_DECIDE_RESTRICT, "missing target -> restrict");
    CHECK(count_event(SCDE_AUDIT_POLICY_FALLBACK) > bad_before,
          "fallback audited");

    /* known-bad hash: DENY + stats + audit */
    scde_security_stats(&before);
    bad_before = count_event(SCDE_AUDIT_PROCESS_BLOCKED);
    d = SCDE_DECIDE_ALLOW;
    CHECK_INT(scde_security_gate_exec(payload, &d), SCDE_OK, "gate bad");
    CHECK_INT(d, SCDE_DECIDE_DENY, "known-bad -> deny");
    scde_security_stats(&after);
    CHECK(after.denied > before.denied, "denied stat incremented");
    CHECK(count_event(SCDE_AUDIT_PROCESS_BLOCKED) > bad_before,
          "PROCESS_BLOCKED audited");
}

/* ------------------------------------------------------------- spawn --- */

static void test_spawn(void)
{
    scde_scan_stats_t before, after;
    int blk_before;
    char allow_sh[300];
    char content[320];

    /* restricted execution still runs the command */
    snprintf(allow_sh, sizeof(allow_sh), "%s/allow.sh", root);
    snprintf(content, sizeof(content), "#!/bin/sh\ntouch %s\n",
             marker_allow);
    write_file(allow_sh, content, 0755);
    unlink(marker_allow);
    spawn(allow_sh);
    CHECK(poll_exists(marker_allow, 3000),
          "restricted spawn created marker");

    /* known-bad target never runs (fail-closed) */
    scde_security_stats(&before);
    blk_before = count_event(SCDE_AUDIT_PROCESS_BLOCKED);
    unlink(marker_sb);
    spawn(payload);
    sleep_ms(400);
    CHECK(!exists(marker_sb), "blocked spawn created nothing");
    scde_security_stats(&after);
    CHECK(after.denied > before.denied, "spawn deny counted");
    CHECK(count_event(SCDE_AUDIT_PROCESS_BLOCKED) > blk_before,
          "spawn deny audited");
}

/* ---------------------------------------------------- sandbox tiers ---- */

static int fn_restrict(void)
{
    int rc = 0;
    pid_t p;

    if (scde_sandbox_apply(SCDE_SANDBOX_RESTRICT) != SCDE_OK)
        return 1;
    if (nnp_status() != 1)
        rc |= 2;
    if (socket(AF_INET, SOCK_STREAM, 0) < 0)
        rc |= 4;                        /* net must stay available */
    p = fork();
    if (p == 0)
        _exit(0);
    if (p < 0)
        rc |= 8;                        /* fork must stay available */
    else {
        int st = 0;

        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            rc |= 16;
    }
    return rc;
}

static int fn_full(void)
{
    scde_security_features_t f;
    int rc = 0;
    int s;

    scde_security_features(&f);
    if (scde_sandbox_apply(SCDE_SANDBOX_FULL) != SCDE_OK)
        return 1;
    if (nnp_status() != 1)
        rc |= 2;
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        close(s);
        rc |= 4;                        /* inet socket must be denied */
    }
    if (fork() >= 0)
        rc |= 8;                        /* fork must be denied */
    if (f.fs == SCDE_FEATURE_AVAILABLE) {
        if (access("/etc/hosts", R_OK) == 0)
            rc |= 16;                   /* hidden behind tmpfs */
        if (access("/etc/resolv.conf", R_OK) == 0)
            rc |= 32;
    }
    if (access("/bin/sh", X_OK) != 0)
        rc |= 64;                       /* system binaries stay usable */
    return rc;
}

static void test_sandbox(void)
{
    scde_security_features_t f;

    scde_security_features(&f);
    CHECK_INT(child_run(fn_restrict), 0, "RESTRICT tier (nnp, net, fork)");
    if (f.sandbox == SCDE_FEATURE_UNAVAILABLE) {
        fprintf(stderr, "note: sandbox UNAVAILABLE, FULL tier skipped\n");
        return;
    }
    CHECK_INT(child_run(fn_full), 0, "FULL tier (seccomp + fs hiding)");
}

/* ------------------------------------------------------- plugin gate --- */

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    int out;
    char buf[8192];
    ssize_t n;

    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0)
        (void)!write(out, buf, (size_t)n);
    close(in);
    close(out);
    return 0;
}

static void test_plugin_gate(const char *example)
{
    char *real_ex = NULL;
    char copy1[300], copy2[300];
    scde_scan_result_t r;
    scde_scan_stats_t before, after;

    snprintf(copy1, sizeof(copy1), "%s/untrusted-copy.so", root);
    snprintf(copy2, sizeof(copy2), "%s/blocked-copy.so", root);
    CHECK_INT(copy_file(example, copy1), 0, "copy 1");
    CHECK_INT(copy_file(example, copy2), 0, "copy 2");
    real_ex = realpath(example, NULL);
    CHECK(real_ex != NULL, "realpath example");

    /* untrusted native code: refused in-process (TRUST_REQUIRED) */
    scde_security_stats(&before);
    memset(&r, 0, sizeof(r));
    CHECK(scde_security_check_plugin(copy1, &r) != SCDE_OK,
          "untrusted plugin refused");
    scde_security_stats(&after);
    CHECK(after.rejected_plugins > before.rejected_plugins,
          "rejection counted");

    /* explicitly blocked: refused */
    CHECK_INT(scde_trust_set(copy2, SCDE_TRUST_BLOCKED), SCDE_OK,
              "trust blocked copy");
    memset(&r, 0, sizeof(r));
    CHECK(scde_security_check_plugin(copy2, &r) != SCDE_OK,
          "blocked plugin refused");

    /* trusted: accepted */
    CHECK_INT(scde_trust_set(real_ex, SCDE_TRUST_TRUSTED), SCDE_OK,
              "trust example");
    memset(&r, 0, sizeof(r));
    CHECK_INT(scde_security_check_plugin(real_ex, &r), SCDE_OK,
              "trusted plugin accepted");
    CHECK(r.trust == SCDE_TRUST_TRUSTED, "result trust trusted");

    /* even trusted: known-bad content still refused */
    CHECK_INT(scde_trust_set(payload, SCDE_TRUST_TRUSTED), SCDE_OK,
              "trust known-bad payload");
    memset(&r, 0, sizeof(r));
    CHECK(scde_security_check_plugin(payload, &r) != SCDE_OK,
          "known-bad refused despite trust");
    free(real_ex);
}

/* ------------------------------------------------------------ audit ---- */

static void test_audit(void)
{
    int i, n;
    int found = 0;
    scde_audit_entry_t e;

    CHECK(scde_audit_count() > 0, "audit has entries");
    n = scde_audit_count();
    for (i = 0; i < n; i++) {
        if (scde_audit_get(i, &e) != SCDE_OK)
            continue;
        if (e.event == SCDE_AUDIT_PROCESS_BLOCKED ||
            e.event == SCDE_AUDIT_TRUST_REQUIRED ||
            e.event == SCDE_AUDIT_POLICY_FALLBACK)
            found++;
    }
    CHECK(found > 0, "gate events visible in ring");
    for (i = SCDE_AUDIT_FILE_SCANNED; i < SCDE_AUDIT_EVENT_COUNT; i++) {
        const char *nm = scde_audit_event_name(i);

        CHECK(nm != NULL && strcmp(nm, "UNKNOWN") != 0, "event name known");
    }
    CHECK(strcmp(scde_audit_event_name(0), "UNKNOWN") == 0,
          "event 0 stays UNKNOWN");
    CHECK_INT(scde_audit_total(SCDE_AUDIT_FILE_SCANNED) > 0, 1,
              "scan counter grows");
    CHECK(scde_security_db_dir() != NULL, "db dir known");
}

/* -------------------------------------------------- trust lifecycle ---- */

static void test_trust_lifecycle(void)
{
    char subj[300];
    scde_scan_result_t r;
    scde_decision_t d;

    snprintf(subj, sizeof(subj), "%s/trust-me.sh", root);
    write_file(subj, "#!/bin/sh\nexit 0\n", 0755);

    /* default: unknown -> UNTRUSTED-family handling (restrict/sandbox) */
    CHECK_INT(scde_trust_get(subj), SCDE_TRUST_UNKNOWN, "default unknown");
    res_init(&r);
    r.trust = SCDE_TRUST_UNKNOWN;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "unknown decide rc");
    CHECK_INT(d, SCDE_DECIDE_RESTRICT, "unknown -> restrict");

    /* trusted -> allow */
    CHECK_INT(scde_trust_set(subj, SCDE_TRUST_TRUSTED), SCDE_OK,
              "trust trusted");
    CHECK_INT(scde_trust_get(subj), SCDE_TRUST_TRUSTED, "trust read back");
    res_init(&r);
    r.trust = SCDE_TRUST_TRUSTED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "trusted decide rc");
    CHECK_INT(d, SCDE_DECIDE_ALLOW, "trusted -> allow");

    /* blocked -> refuse */
    CHECK_INT(scde_trust_set(subj, SCDE_TRUST_BLOCKED), SCDE_OK,
              "trust blocked");
    res_init(&r);
    r.trust = SCDE_TRUST_BLOCKED;
    CHECK_INT(scde_security_decide(&r, &d), SCDE_OK, "blocked decide rc");
    CHECK_INT(d, SCDE_DECIDE_DENY, "blocked -> deny");
    CHECK(scde_security_check_plugin(subj, NULL) != SCDE_OK,
          "blocked plugin refused");

    /* forget -> back to unknown */
    CHECK_INT(scde_trust_forget(subj), SCDE_OK, "trust forget");
    CHECK_INT(scde_trust_get(subj), SCDE_TRUST_UNKNOWN,
              "forgotten -> unknown");
}

/* --------------------------------------- mediated capabilities --------- */

static void test_mediated_caps(void)
{
    char out[128];
    const char *enc;

    scde_security_revoke("test:med", 0xffffffffu);
    scde_security_revoke("test:med-ok", 0xffffffffu);

    /* forbidden capability -> denied (fail-closed + audited) */
    CHECK(scde_service_call_as("test:med", "notify", "t\nb", out,
                               sizeof(out)) != SCDE_OK,
          "notify without AUDIT denied");
    CHECK(scde_service_call_as("test:med", "config.get", "k", out,
                               sizeof(out)) != SCDE_OK,
          "config.get without CONFIG denied");
    CHECK_INT(scde_service_call_as("test:med", "version", NULL, out,
                                   sizeof(out)), SCDE_OK,
              "version is public");

    /* granted -> allowed */
    CHECK_INT(scde_security_grant("test:med-ok", SCDE_CAP_AUDIT), SCDE_OK,
              "grant audit");
    CHECK_INT(scde_service_call_as("test:med-ok", "notify", "hi\nthere",
                                   out, sizeof(out)), SCDE_OK,
              "notify with AUDIT allowed");
    scde_security_revoke("test:med-ok", 0xffffffffu);

    /* enforcement labels are honest (never claim kernel for userspace) */
    enc = scde_capability_enforcement(SCDE_CAP_CONFIG);
    CHECK(enc != NULL && (strcmp(enc, "BYPASSABLE") == 0 ||
                          strcmp(enc, "POLICY-LEVEL ONLY") == 0),
          "config is userspace-only");
    enc = scde_capability_enforcement(SCDE_CAP_NET);
    CHECK(enc != NULL && *enc != '\0', "net has a label");
    CHECK(strcmp(scde_capability_enforcement(0), "none") == 0,
          "none label");
}

/* -------------------------------------------------------- resources ---- */

static int fn_limits_child(void)
{
    struct rlimit rl;

    if (scde_resource_apply() != SCDE_OK)
        return 1;
    if (getrlimit(RLIMIT_CORE, &rl) != 0)
        return 2;
    if (rl.rlim_cur != 0)
        return 3;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 4;
    if (rl.rlim_cur == 0 || rl.rlim_cur > 4096)
        return 5;
#ifdef RLIMIT_AS
    if (getrlimit(RLIMIT_AS, &rl) != 0)
        return 6;
    if (rl.rlim_cur == 0 || rl.rlim_cur > (rlim_t)(2UL * 1024UL * 1024UL * 1024UL))
        return 7;
#endif
    if (getrlimit(RLIMIT_CPU, &rl) != 0)
        return 8;
    if (rl.rlim_cur == 0 || rl.rlim_cur > 3600)
        return 9;
    if (getrlimit(RLIMIT_FSIZE, &rl) != 0)
        return 10;
    if (rl.rlim_cur == 0)
        return 11;
    return 0;
}

static void test_resources(void)
{
    scde_resource_limits_t lim;
    char desc[128];
    pid_t victim;
    pid_t me;

    scde_resource_defaults(&lim);
    CHECK(lim.ram_bytes > 0, "default ram set");
    CHECK(lim.cpu_seconds > 0, "default cpu set");
    CHECK(lim.nproc > 0, "default nproc set");
    CHECK(lim.nofile > 0, "default nofile set");
    CHECK(lim.fsize_bytes > 0, "default fsize set");
    CHECK_INT(scde_resource_get(&lim), SCDE_OK, "resource get");
    CHECK_INT(scde_resource_describe(desc, sizeof(desc)), SCDE_OK,
              "resource describe");
    CHECK(desc[0] != '\0', "describe non-empty");

    /* custom limits round-trip */
    lim.nofile = 64;
    lim.nproc = 16;
    CHECK_INT(scde_resource_set(&lim), SCDE_OK, "resource set custom");
    CHECK_INT(child_run(fn_limits_child), 0, "child limits applied");
    CHECK_INT(scde_resource_set(NULL), SCDE_OK, "resource reset");

    /* supervisor only kills the target: SCDE (this process) survives */
    me = getpid();
    victim = fork();
    if (victim == 0) {
        for (;;)
            sleep_ms(50);
        _exit(0);
    }
    CHECK(victim > 0, "victim forked");
    if (victim > 0) {
        int st = 0;

        CHECK_INT(scde_resource_constrain(victim, 100, &st), 1,
                  "victim reaped via SIGTERM/SIGKILL");
        CHECK_INT(kill(victim, 0) == 0 && errno != ESRCH ? 1 : 0, 0,
                  "victim is gone");
        CHECK_INT((int)(getpid() == me), 1, "supervisor survived");
        CHECK(kill(getpid(), 0) == 0, "SCDE alive after termination");
    }
    /* never touch ourselves or init */
    CHECK_INT(scde_resource_constrain(getpid(), 50, NULL), 0,
              "refuse self-kill");
    CHECK_INT(scde_resource_constrain((pid_t)1, 50, NULL), 0,
              "refuse pid 1");
}

/* ----------------------------------------------------- bait + mirage --- */

static void test_bait_mirage(void)
{
    char bdir[300];
    char dpath[SCDE_BAIT_PATH_LEN];
    char mval[128];
    int acc_before, mod_before, del_before, exec_before;
    int mp_before, ma_before, esc_before;

    snprintf(bdir, sizeof(bdir), "%s/bait", root);
    mkdir(bdir, 0700);

    CHECK_INT(scde_bait_init(bdir), SCDE_OK, "bait init");
    CHECK(scde_bait_active(), "bait active");
    CHECK_INT(scde_bait_create_defaults(), SCDE_OK, "bait defaults");
    CHECK_INT(scde_bait_count(), 5, "five decoys");
    CHECK_INT(scde_bait_verify_dummy(), SCDE_OK, "decoys are dummy-only");

    /* explicit access telemetry */
    CHECK_INT(scde_bait_path(0, dpath, sizeof(dpath)), SCDE_OK,
              "bait path 0");
    acc_before = (int)scde_audit_total(SCDE_AUDIT_BAIT_ACCESSED);
    CHECK_INT(scde_bait_report(dpath, "read", SCDE_TRUST_UNTRUSTED),
              SCDE_OK, "bait report read");
    CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_ACCESSED) > acc_before,
          "BAIT_ACCESSED audited");
    exec_before =
        (int)scde_audit_total(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT);
    CHECK_INT(scde_bait_report(dpath, "exec", SCDE_TRUST_UNTRUSTED),
              SCDE_OK, "bait report exec");
    CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT) >
          exec_before, "BAIT_EXECUTION_ATTEMPT audited");
    CHECK(scde_bait_hits() >= 2, "bait hits counted");

    /* modification detected by polling */
    mod_before = (int)scde_audit_total(SCDE_AUDIT_BAIT_MODIFIED);
    {
        int fd = open(dpath, O_WRONLY | O_APPEND);

        if (fd >= 0) {
            (void)!write(fd, "x", 1);
            close(fd);
        }
    }
    sleep_ms(50);
    CHECK(scde_bait_check() > 0, "bait check found modification");
    CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_MODIFIED) > mod_before,
          "BAIT_MODIFIED audited");

    /* exec-bit attempt: detected and neutralised (back to non-exec) */
    chmod(dpath, 0755);
    exec_before =
        (int)scde_audit_total(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT);
    CHECK(scde_bait_check() > 0, "bait check found exec bit");
    CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT) >
          exec_before, "exec-bit audited");
    {
        struct stat st;

        if (stat(dpath, &st) == 0)
            CHECK((st.st_mode & 0111) == 0, "decoy back to non-exec");
    }

    /* deletion detected */
    del_before = (int)scde_audit_total(SCDE_AUDIT_BAIT_DELETED);
    unlink(dpath);
    CHECK(scde_bait_check() > 0, "bait check found deletion");
    CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_DELETED) > del_before,
          "BAIT_DELETED audited");

    /* symlink swap: a link planted where a decoy was must be reported but
       never chmod'ed through - the link target keeps its mode/content */
    {
        char victim[300];
        char vcontent[] = "victim-data";
        char rback[32];
        int vfd;
        struct stat vst;
        int mod2_before;

        snprintf(victim, sizeof(victim), "%s/victim.txt", root);
        vfd = open(victim, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (vfd >= 0) {
            (void)!write(vfd, vcontent, sizeof(vcontent) - 1);
            close(vfd);
        }
        chmod(victim, 0755);
        CHECK_INT(scde_bait_path(1, dpath, sizeof(dpath)), SCDE_OK,
                  "second decoy path");
        unlink(dpath);
        CHECK_INT(symlink(victim, dpath), 0, "plant decoy symlink");
        mod2_before =
            (int)scde_audit_total(SCDE_AUDIT_BAIT_MODIFIED);
        CHECK(scde_bait_check() > 0, "bait check found symlink swap");
        CHECK((int)scde_audit_total(SCDE_AUDIT_BAIT_MODIFIED) >
              mod2_before, "symlink swap audited");
        CHECK(stat(victim, &vst) == 0 &&
              (vst.st_mode & 07777) == 0755,
              "symlink target mode untouched");
        vfd = open(victim, O_RDONLY);
        if (vfd >= 0) {
            ssize_t vn = read(vfd, rback, sizeof(rback) - 1);

            close(vfd);
            rback[vn > 0 ? vn : 0] = '\0';
            CHECK(strcmp(rback, vcontent) == 0,
                  "symlink target content untouched");
        } else {
            CHECK(0, "symlink target content untouched");
        }
        unlink(dpath);
        unlink(victim);
    }

    /* disabled bait is inert to polling (explicit exec reports still
     * fail-closed and audited, polling stays silent) */
    CHECK_INT(scde_bait_disable(), SCDE_OK, "bait disable");
    CHECK_INT(scde_bait_enabled(), 0, "bait off");
    CHECK_INT(scde_bait_check(), 0, "disabled check silent");
    CHECK_INT(scde_bait_enable(), SCDE_OK, "bait enable");
    CHECK_INT(scde_bait_enabled(), 1, "bait on");

    /* mirage: every probe denied + audited, dummy value only */
    CHECK_INT(scde_mirage_init(), SCDE_OK, "mirage init");
    CHECK_INT(scde_mirage_create_defaults(), SCDE_OK, "mirage defaults");
    CHECK_INT(scde_mirage_count(), 4, "four mirages");
    mp_before = (int)scde_audit_total(SCDE_AUDIT_MIRAGE_PROBED);
    CHECK(scde_mirage_probe("SCDE_DECOY_SESSION", "probe",
                            SCDE_TRUST_UNTRUSTED) != SCDE_OK,
          "mirage probe denied");
    CHECK((int)scde_audit_total(SCDE_AUDIT_MIRAGE_PROBED) > mp_before,
          "MIRAGE_PROBED audited");
    ma_before = (int)scde_audit_total(SCDE_AUDIT_MIRAGE_ACCESSED);
    memset(mval, 0, sizeof(mval));
    CHECK(scde_mirage_lookup("decoy-gpu0", "read", SCDE_TRUST_UNTRUSTED,
                             mval, sizeof(mval)) != SCDE_OK,
          "mirage read denied");
    CHECK((int)scde_audit_total(SCDE_AUDIT_MIRAGE_ACCESSED) > ma_before,
          "MIRAGE_ACCESSED audited");
    CHECK(strstr(mval, "decoy") != NULL, "mirage value is dummy");
    CHECK(strstr(mval, "AKIA") == NULL, "mirage value has no secrets");
    esc_before =
        (int)scde_audit_total(SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT);
    CHECK(scde_mirage_probe("decoy-admin-token", "escape",
                            SCDE_TRUST_UNTRUSTED) != SCDE_OK,
          "mirage escape denied");
    CHECK((int)scde_audit_total(SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT) >
          esc_before, "MIRAGE_ESCAPE_ATTEMPT audited");
    CHECK(scde_mirage_probes() >= 3, "mirage probes counted");

    scde_mirage_shutdown();
    CHECK_INT(scde_mirage_active(), 0, "mirage shut down");
    scde_bait_shutdown();
    CHECK_INT(scde_bait_active(), 0, "bait shut down");
}

/* --------------------------------------------- symlink cache safety --- */

static void test_scan_symlink_cache(void)
{
    char target[300];
    char link[300];
    scde_scan_result_t r1, r2;

    snprintf(target, sizeof(target), "%s/link-target.sh", root);
    snprintf(link, sizeof(link), "%s/link-scan.sh", root);
    write_file(target, "#!/bin/sh\necho harmless\n", 0755);
    unlink(link);
    CHECK_INT(symlink(target, link), 0, "create scan symlink");

    /* first scan through the link: fresh verdict, never served cached */
    memset(&r1, 0, sizeof(r1));
    CHECK_INT(scde_scan_file(link, &r1), SCDE_OK, "scan via symlink");
    CHECK((r1.flags & SCDE_SCANFLAG_CACHED) == 0,
          "symlink scan never cached");
    CHECK(r1.status != SCDE_THREAT_BLOCKED, "safe target not blocked");

    /* swap the target content in place (the link itself never changes):
       a rescan must see the new content, not a stale cached verdict */
    write_file(target, "#!/bin/sh\ntouch /tmp/pwned\nrm -rf /\n", 0755);
    memset(&r2, 0, sizeof(r2));
    CHECK_INT(scde_scan_file(link, &r2), SCDE_OK, "rescan via symlink");
    CHECK((r2.flags & SCDE_SCANFLAG_CACHED) == 0,
          "symlink rescan never cached");
    CHECK(r2.risk > r1.risk, "swapped target raises risk through symlink");
    CHECK(r2.status == SCDE_THREAT_MALICIOUS ||
          r2.status == SCDE_THREAT_BLOCKED,
          "destructive target flagged through symlink");
    unlink(link);
    unlink(target);
}

/* ------------------------------------------------------------ main ----- */

int main(int argc, char **argv)
{
    char dbdir[300];
    char hashes[340];
    char content[512];
    char hex[65];
    scde_scan_result_t r;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <example.so>\n", argv[0]);
        return 2;
    }
    sc.lock_fd = -1;                    /* spawn closes >=0 fds only */

    snprintf(root, sizeof(root), "/tmp/scde-secXXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(dbdir, sizeof(dbdir), "%s/db", root);
    mkdir(dbdir, 0755);
    setenv("SCDE_SECURITY_DIR", dbdir, 1);
    setenv("SCDE_SCAN_MODE", "always-on", 1);

    snprintf(payload, sizeof(payload), "%s/payload.sh", root);
    snprintf(marker_allow, sizeof(marker_allow), "%s/marker_allow", root);
    snprintf(marker_sb, sizeof(marker_sb), "%s/marker_sb", root);
    snprintf(content, sizeof(content),
             "#!/bin/sh\ntouch %s\n", marker_sb);
    write_file(payload, content, 0755);

    CHECK_INT(scde_security_init(dbdir), SCDE_OK, "security init");

    /* known-bad database: hash of the payload script */
    CHECK_INT(scde_hash_file(payload, hex), SCDE_OK, "hash payload");
    snprintf(hashes, sizeof(hashes), "%s/hashes.db", dbdir);
    {
        char line[160];

        snprintf(line, sizeof(line),
                 "# scde security\n%s  security-test\n", hex);
        write_file(hashes, line, 0644);
    }
    CHECK_INT(scde_security_init(dbdir), SCDE_OK, "security re-init (db)");

    {
        int rules = 0, hashes_n = 0, trusted = 0;

        scde_security_db_info(&rules, &hashes_n, &trusted);
        fprintf(stderr, "DB INFO: rules=%d hashes=%d trusted=%d\\n",
               rules, hashes_n, trusted);
    }

    test_features();
    test_decide();
    test_capabilities();
    test_trust_lifecycle();

    {
        scde_app_info_t info;

        memset(&info, 0, sizeof(info));
        info.name = "security-test";
        info.version = "1.0";
        if (scde_app_init(&info) != SCDE_OK &&
            !scde_app_is_running()) {
            CHECK(0, "app init for mediated services");
        } else {
            test_mediated_caps();
        }
    }

    /* scanner really flags the known-bad payload */
    memset(&r, 0, sizeof(r));
    CHECK_INT(scde_scan_file(payload, &r), SCDE_OK, "scan payload");
    CHECK_INT(r.status, SCDE_THREAT_BLOCKED, "known-bad scanned blocked");
    CHECK_INT(r.hash_status, SCDE_HASH_KNOWN_BAD, "known-bad hash status");

    test_scan_symlink_cache();
    test_gate();
    test_spawn();
    test_sandbox();
    test_resources();
    test_bait_mirage();
    test_plugin_gate(argv[1]);
    test_audit();
    scde_app_shutdown();

    {
        scde_security_features_t f;

        scde_security_features(&f);
        printf("environment: kernel=%s sandbox=%s fs=%s seccomp=%s "
               "proot=%d\n",
               scde_feature_state_name(f.kernel_enforcement),
               scde_feature_state_name(f.sandbox),
               scde_feature_state_name(f.fs),
               scde_feature_state_name(f.seccomp), f.proot);
    }
    printf("security tests: %d checks, %d failures\n", checks, fails);

    /* leave no residue */
    {
        char cmd[320];

        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
        if (system(cmd) != 0)
            fprintf(stderr, "warning: cleanup failed for %s\n", root);
    }
    return fails ? 1 : 0;
}
