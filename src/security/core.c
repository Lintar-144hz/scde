/*
 * SCDE Security Layer - core: lifecycle, path validation, trust state,
 * statistics and the plugin gate.
 */
#include <scde/plugin.h>
#include <scde/security.h>

#include "security_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int inited;
static int initing;
static char db_dir[512];
static scde_scan_stats_t stats;

typedef struct {
    int used;
    char path[SCDE_PATH_MAX];
    int trust;                      /* scde_trust_t */
} trust_entry_t;

static trust_entry_t trust_table[SCDE_TRUST_MAX];

scde_scan_stats_t *scde__stats(void)
{
    return &stats;
}

/* ------------------------------------------------------------ lifecycle -- */

static void resolve_default_dir(char *out, size_t out_len)
{
    const char *env = getenv("SCDE_SECURITY_DIR");
    const char *home = getenv("HOME");

    if (env && *env) {
        snprintf(out, out_len, "%.*s", (int)(out_len - 1), env);
        return;
    }
    if (home && *home) {
        char probe[400];

        snprintf(probe, sizeof(probe), "%.*s/.config/scde/security",
                 (int)(sizeof(probe) - 24), home);
        if (access(probe, F_OK) == 0) {
            snprintf(out, out_len, "%.*s", (int)(out_len - 1), probe);
            return;
        }
    }
    snprintf(out, out_len, "%s", "/etc/scde/security");
}

int scde_security_init(const char *dir)
{
    if (initing)
        return SCDE_ERR_STATE;
    initing = 1;
    if (dir && *dir)
        snprintf(db_dir, sizeof(db_dir), "%.*s",
                 (int)(sizeof(db_dir) - 1), dir);
    else if (!db_dir[0])
        resolve_default_dir(db_dir, sizeof(db_dir));

    memset(&stats, 0, sizeof(stats));
    if (!inited) {
        memset(trust_table, 0, sizeof(trust_table));
        inited = 1;
    }
    initing = 0;
    scde__scanner_init(db_dir);       /* DB load (inited already set above) */

    {
        const char *m = getenv("SCDE_SCAN_MODE");

        if (m && *m) {
            if (strcmp(m, "always-on") == 0 || strcmp(m, "always") == 0 ||
                strcmp(m, "1") == 0)
                scde_security_set_scan_mode(SCDE_SCAN_ALWAYS_ON);
            else if (strcmp(m, "off") == 0 || strcmp(m, "0") == 0)
                scde_security_set_scan_mode(SCDE_SCAN_OFF);
            else
                scde_security_set_scan_mode(SCDE_SCAN_PASSIVE);
        }
    }
    return SCDE_OK;
}

void scde__ensure(void)
{
    if (inited || initing)
        return;
    scde_security_init(NULL);
}

void scde_security_shutdown(void)
{
    if (!inited)
        return;
    scde__scanner_shutdown();
    memset(trust_table, 0, sizeof(trust_table));
    memset(&stats, 0, sizeof(stats));
    db_dir[0] = '\0';
    inited = 0;
}

const char *scde_security_db_dir(void)
{
    scde__ensure();
    return db_dir;
}

int scde_security_reload_db(void)
{
    scde__ensure();
    return scde__scanner_reload();
}

/* ---------------------------------------------------------- path safety -- */

int scde_path_is_safe(const char *path)
{
    size_t len;
    size_t i;

    if (!path || !*path)
        return SCDE_ERR_ARG;
    len = strlen(path);
    if (len >= SCDE_PATH_MAX)
        return SCDE_ERR_ARG;
    if (path[0] != '/')                 /* absolute only: no relative tricks */
        return SCDE_ERR_ARG;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];

        if (c < 0x20 || c == 0x7f)      /* control characters */
            return SCDE_ERR_ARG;
    }
    /* no ".." component anywhere */
    for (i = 0; i + 1 < len; i++) {
        if (path[i] == '.' && path[i + 1] == '.' &&
            (i == 0 || path[i - 1] == '/') &&
            (i + 2 >= len || path[i + 2] == '/' || path[i + 2] == '\0'))
            return SCDE_ERR;
    }
    return SCDE_OK;
}

/* ---------------------------------------------------------------- trust -- */

static trust_entry_t *trust_slot(const char *path)
{
    int free_slot = -1;
    int i;

    for (i = 0; i < SCDE_TRUST_MAX; i++) {
        if (!trust_table[i].used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (strcmp(trust_table[i].path, path) == 0)
            return &trust_table[i];
    }
    if (free_slot < 0)
        return NULL;
    trust_table[free_slot].used = 1;
    snprintf(trust_table[free_slot].path, sizeof(trust_table[free_slot].path),
             "%.*s", SCDE_PATH_MAX - 1, path);
    trust_table[free_slot].trust = SCDE_TRUST_UNKNOWN;
    return &trust_table[free_slot];
}

int scde_trust_get(const char *path)
{
    int i;

    if (scde_path_is_safe(path) != SCDE_OK)
        return SCDE_TRUST_UNKNOWN;
    for (i = 0; i < SCDE_TRUST_MAX; i++) {
        if (trust_table[i].used && strcmp(trust_table[i].path, path) == 0)
            return trust_table[i].trust;
    }
    return SCDE_TRUST_UNKNOWN;
}

int scde_trust_set(const char *path, int trust)
{
    trust_entry_t *e;

    if (scde_path_is_safe(path) != SCDE_OK)
        return SCDE_ERR_ARG;
    if (trust != SCDE_TRUST_TRUSTED && trust != SCDE_TRUST_UNTRUSTED &&
        trust != SCDE_TRUST_BLOCKED && trust != SCDE_TRUST_UNKNOWN)
        return SCDE_ERR_ARG;
    e = trust_slot(path);
    if (!e)
        return SCDE_ERR_FULL;
    e->trust = trust;
    return SCDE_OK;
}

int scde_trust_forget(const char *path)
{
    int i;

    if (scde_path_is_safe(path) != SCDE_OK)
        return SCDE_ERR_ARG;
    for (i = 0; i < SCDE_TRUST_MAX; i++) {
        if (trust_table[i].used && strcmp(trust_table[i].path, path) == 0) {
            memset(&trust_table[i], 0, sizeof(trust_table[i]));
            return SCDE_OK;
        }
    }
    return SCDE_ERR_NOT_FOUND;
}

/* --------------------------------------------------------- plugin gate -- */

/*
 * Required order (docs/SECURITY.md):
 *   path validation -> scan -> trust -> policy -> capability setup (manager)
 *   -> sandbox/process decision -> ABI validation (loader) -> init.
 *
 * In-process dlopen of native code is TRUSTED-ONLY: constructors and init
 * run inside the SCDE process, so there is no isolation to claim.  Loading
 * an untrusted plugin requires the explicit unsafe policy override, and
 * even then it gets no capabilities (manager.c).
 */
int scde_security_check_plugin(const char *path, scde_scan_result_t *result)
{
    scde_scan_result_t local;
    scde_scan_result_t *out = result ? result : &local;
    scde_decision_t dec;
    int trust;
    char subject[80];

    memset(out, 0, sizeof(*out));
    out->status = SCDE_THREAT_UNKNOWN;
    out->trust = SCDE_TRUST_UNKNOWN;
    scde__ensure();

    snprintf(subject, sizeof(subject), "%.*s",
             (int)(sizeof(subject) - 1), path ? path : "-");

    if (!path || !*path || scde_path_is_safe(path) != SCDE_OK) {
        scde__stats()->rejected_plugins++;
        scde_audit_log(SCDE_AUDIT_PLUGIN_REJECTED, subject, 90,
                       "unsafe plugin path");
        return SCDE_ERR;
    }

    /* scan (no-op when the mode is OFF: trust + policy still apply) */
    {
        int st = scde_scan_file(path, out);

        if (st != SCDE_OK && st != SCDE_ERR_STATE) {
            scde__stats()->rejected_plugins++;
            scde_audit_log(SCDE_AUDIT_PLUGIN_REJECTED, subject, 0,
                           "plugin path unusable");
            return SCDE_ERR;
        }
    }

    /* trust: in-process native code requires TRUSTED */
    trust = scde_trust_get(path);
    out->trust = (int16_t)trust;
    if (trust == SCDE_TRUST_BLOCKED) {
        scde__stats()->rejected_plugins++;
        scde_audit_log(SCDE_AUDIT_PLUGIN_REJECTED, subject, 100,
                       "plugin explicitly blocked");
        return SCDE_ERR;
    }
    if (trust != SCDE_TRUST_TRUSTED) {
        if (scde_policy_get(SCDE_POL_REQUIRE_TRUSTED_PLUGINS)) {
            scde__stats()->rejected_plugins++;
            scde__stats()->denied++;
            scde_audit_log(SCDE_AUDIT_TRUST_REQUIRED, subject, 80,
                           "plugin is not trusted");
            return SCDE_ERR;
        }
        if (!scde_policy_get(SCDE_POL_ALLOW_UNTRUSTED_PLUGINS)) {
            scde__stats()->rejected_plugins++;
            scde__stats()->denied++;
            scde_audit_log(SCDE_AUDIT_TRUST_REQUIRED, subject, 80,
                           "in-process load requires TRUSTED");
            return SCDE_ERR;
        }
        /* explicit unsafe override: runs untrusted native code in-process */
        scde_audit_log(SCDE_AUDIT_POLICY_FALLBACK, subject, 60,
                       "untrusted plugin allowed by policy");
    }

    /* scanner/policy decision (fail-closed on error) */
    if (scde_security_decide(out, &dec) != SCDE_OK || dec == SCDE_DECIDE_DENY) {
        scde__stats()->rejected_plugins++;
        scde__stats()->denied++;
        scde_audit_log(SCDE_AUDIT_PLUGIN_REJECTED, subject, out->risk,
                       "plugin failed the policy gate");
        return SCDE_ERR;
    }

    if (trust == SCDE_TRUST_TRUSTED)
        scde_audit_log(SCDE_AUDIT_PLUGIN_TRUSTED, path, 0, "trust check ok");
    return SCDE_OK;
}

/* ----------------------------------------------------------- exec gate -- */

/* subject of the most recent scde_security_gate_exec() call (for spawn) */
static char last_exec_subject[SCDE_PATH_MAX];

const char *scde__last_exec_subject(void)
{
    return last_exec_subject;
}

/*
 * First executable word of a command: skips spaces, honours quotes and
 * skips leading VAR=value assignments.  Returns 0 when nothing is found.
 */
static int first_word(const char *cmd, char *out, size_t out_len)
{
    const char *p = cmd;
    size_t n = 0;

    if (!cmd || !out || out_len == 0)
        return 0;
    for (;;) {
        const char *start;
        int quoted = 0;
        int is_assign = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        start = p;
        n = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '\'' || *p == '"') {
                char q = *p;

                if (!quoted) {
                    quoted = q;
                    p++;
                    continue;
                }
                if (quoted == q) {
                    quoted = 0;
                    p++;
                    continue;
                }
            }
            if (n + 1 < out_len)
                out[n++] = *p;
            p++;
        }
        out[n] = '\0';
        if (n == 0)
            break;
        /* NAME=value in front of the real command: skip this word */
        {
            const char *q = start;

            while (q < p && *q != '/' && *q != '=')
                q++;
            is_assign = (*q == '=' && q > start);
        }
        if (!is_assign)
            return 1;
    }
    out[0] = '\0';
    return 0;
}

/* absolute/relative path or $PATH lookup -> canonical path, 0 when unknown.
 * realpath() needs a PATH_MAX-capable buffer (FORTIFY aborts otherwise),
 * so resolve on the heap and copy back bounded; over-long results are
 * treated as unresolvable (fail-closed restricted fallback). */
static int copy_resolved(char *out, size_t out_len, char *rp)
{
    size_t n;

    if (!rp)
        return 0;
    n = strlen(rp);
    if (n == 0 || n >= out_len) {
        free(rp);
        return 0;
    }
    memcpy(out, rp, n + 1);
    free(rp);
    return 1;
}

static int resolve_target(const char *word, char *out, size_t out_len)
{
    const char *path_env;
    char cand[SCDE_PATH_MAX];

    if (!word || !*word || !out || out_len == 0)
        return 0;

    if (strchr(word, '/')) {
        if (copy_resolved(out, out_len, realpath(word, NULL)))
            return 1;
        if (word[0] == '/' && strlen(word) < out_len) {
            snprintf(out, out_len, "%s", word);
            return 1;                   /* best effort for missing files */
        }
        return 0;
    }

    path_env = getenv("PATH");
    if (!path_env || !*path_env)
        path_env = "/usr/local/bin:/usr/bin:/bin";
    while (*path_env) {
        const char *sep = strchr(path_env, ':');
        size_t dlen = sep ? (size_t)(sep - path_env) : strlen(path_env);

        if (dlen > 0 && dlen + 1 + strlen(word) + 1 < sizeof(cand)) {
            int w = snprintf(cand, sizeof(cand), "%.*s/%s",
                             (int)dlen, path_env, word);

            if (w > 0 && (size_t)w < sizeof(cand) && access(cand, X_OK) == 0) {
                if (copy_resolved(out, out_len, realpath(cand, NULL)))
                    return 1;
                snprintf(out, out_len, "%s", cand);
                return 1;
            }
        }
        if (!sep)
            break;
        path_env = sep + 1;
    }
    return 0;
}

/*
 * Policy gate for spawning a command: resolve -> scan -> trust -> decide.
 * Fail-closed: any error keeps *out = SCDE_DECIDE_DENY.  Audits DENY and
 * unresolved-target fallbacks here; ALLOW/RESTRICT/SANDBOX outcomes are
 * audited by spawn() once the child verified its restrictions.
 */
int scde_security_gate_exec(const char *cmd, scde_decision_t *out)
{
    char word[SCDE_PATH_MAX];
    char target[SCDE_PATH_MAX];
    scde_scan_result_t r;
    int have_target;
    int have_word;

    if (!out)
        return SCDE_ERR_ARG;
    *out = SCDE_DECIDE_DENY;
    if (!cmd || !*cmd)
        return SCDE_ERR_ARG;
    scde__ensure();
    {
        scde_security_features_t f;

        scde_security_features(&f);     /* probe once, reuse in children */
        (void)f;
    }

    have_word = first_word(cmd, word, sizeof(word));
    have_target = have_word &&
                  resolve_target(word, target, sizeof(target));
    if (have_target)
        snprintf(last_exec_subject, sizeof(last_exec_subject), "%.*s",
                 (int)(sizeof(last_exec_subject) - 1), target);
    else if (have_word)
        snprintf(last_exec_subject, sizeof(last_exec_subject), "%.*s",
                 (int)(sizeof(last_exec_subject) - 1), word);
    else
        snprintf(last_exec_subject, sizeof(last_exec_subject), "-");

    if (!have_target) {
        /* unknown target: policy default = restricted execution */
        *out = SCDE_DECIDE_RESTRICT;
        scde_audit_log(SCDE_AUDIT_POLICY_FALLBACK, last_exec_subject, 40,
                       "target not resolvable, restricting");
        return SCDE_OK;
    }

    memset(&r, 0, sizeof(r));
    r.status = SCDE_THREAT_UNKNOWN;
    r.trust = SCDE_TRUST_UNKNOWN;
    r.hash_status = SCDE_HASH_NOT_COMPUTED;
    {
        int st = scde_scan_file(target, &r);

        if (st == SCDE_ERR_STATE) {
            /* scanner off: trust + policy still decide, never "safe" */
            r.status = SCDE_THREAT_UNKNOWN;
            r.trust = (int16_t)scde_trust_get(target);
            r.hash_status = SCDE_HASH_SKIPPED;
            snprintf(r.detail, sizeof(r.detail), "scanner off");
        } else if (st != SCDE_OK) {
            scde__stats()->denied++;
            scde_audit_log(SCDE_AUDIT_PROCESS_BLOCKED, last_exec_subject, 95,
                           "scan failed");
            return SCDE_OK;             /* DENY (fail-closed) */
        }
    }

    if (scde_security_decide(&r, out) != SCDE_OK) {
        *out = SCDE_DECIDE_DENY;
        scde__stats()->denied++;
        scde_audit_log(SCDE_AUDIT_PROCESS_BLOCKED, last_exec_subject, 95,
                       "decision failed");
        return SCDE_OK;
    }

    if (*out == SCDE_DECIDE_DENY) {
        scde_security_features_t f;

        scde_security_features(&f);
        scde__stats()->denied++;
        if (f.sandbox == SCDE_FEATURE_UNAVAILABLE &&
            (r.trust == SCDE_TRUST_UNTRUSTED ||
             r.status == SCDE_THREAT_SUSPICIOUS)) {
            scde_audit_log(SCDE_AUDIT_SECURITY_FEATURE_UNAVAILABLE,
                           last_exec_subject, 80,
                           "sandbox unavailable, refusing target");
        }
        scde_audit_log(SCDE_AUDIT_PROCESS_BLOCKED, last_exec_subject,
                       r.risk ? r.risk : 95, "blocked by policy");
    }
    return SCDE_OK;
}

/* ------------------------------------------------------------ monitoring -- */

int scde_security_watch_dir(const char *dir)
{
    scde__ensure();
    return scde__scanner_watch_dir(dir);
}

int scde_security_watch_defaults(void)
{
    const char *sep;
    const char *p;

    scde__ensure();
    if (scde_security_get_scan_mode() != SCDE_SCAN_ALWAYS_ON)
        return SCDE_ERR_STATE;

    scde_security_watch_dir(db_dir);

    p = scde_plugins_search_path();
    while (p && *p) {
        char dir[SCDE_PATH_MAX];
        size_t dlen;

        sep = strchr(p, ':');
        dlen = sep ? (size_t)(sep - p) : strlen(p);
        if (dlen > 0 && dlen < sizeof(dir)) {
            memcpy(dir, p, dlen);
            dir[dlen] = '\0';
            scde_security_watch_dir(dir);
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return SCDE_OK;
}

void scde_security_watch_stop(void)
{
    scde__scanner_watch_stop();
}

int scde_security_watch_fd(void)
{
    if (!inited || scde_security_get_scan_mode() != SCDE_SCAN_ALWAYS_ON)
        return -1;
    return scde__scanner_watch_fd();
}

int scde_security_pump(void)
{
    if (!inited || scde_security_get_scan_mode() != SCDE_SCAN_ALWAYS_ON)
        return SCDE_ERR_STATE;
    return scde__scanner_pump();
}

/* ---------------------------------------------------------------- stats -- */

void scde_security_stats(scde_scan_stats_t *out)
{
    if (!out)
        return;
    *out = stats;
}
