/*
 * SCDE Security Layer - capabilities, permission checks and process
 * restrictions.
 *
 * Capabilities are per-subject bit masks ("plugin:example", "app:foo",
 * ...).  Unknown subjects hold nothing, so every check fails closed and is
 * audited as CAPABILITY_DENIED.
 *
 * scde_security_restrict_self() applies real kernel knobs (no_new_privs,
 * zero core dumps) to the calling process.  It is the first building block
 * for future plugin subprocess isolation - the session itself is not
 * restricted by default, and no sandbox is claimed before one exists.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>

#include "security_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define SUBJ_LEN 40

typedef struct {
    int used;
    char name[SUBJ_LEN];
    uint32_t caps;
} subject_t;

static subject_t subjects[SCDE_CAP_SUBJECTS];

static const struct {
    const char *name;
    uint32_t cap;
} cap_names[] = {
    { "fs.read", SCDE_CAP_FS_READ },
    { "fs.write", SCDE_CAP_FS_WRITE },
    { "fs.exec", SCDE_CAP_FS_EXEC },
    { "fs.watch", SCDE_CAP_FS_WATCH },
    { "net", SCDE_CAP_NET },
    { "device", SCDE_CAP_DEVICE },
    { "spawn", SCDE_CAP_SPAWN },
    { "config", SCDE_CAP_CONFIG },
    { "audit", SCDE_CAP_AUDIT },
    { "plugin.load", SCDE_CAP_PLUGIN_LOAD }
};

static subject_t *find_subject(const char *name)
{
    int i;

    for (i = 0; i < SCDE_CAP_SUBJECTS; i++) {
        if (subjects[i].used && strcmp(subjects[i].name, name) == 0)
            return &subjects[i];
    }
    return NULL;
}

static void deny(const char *subject, uint32_t caps, const char *why)
{
    scde__stats()->denied++;
    scde_audit_log(SCDE_AUDIT_CAPABILITY_DENIED, subject, 0, why);
    (void)caps;
}

int scde_security_grant(const char *subject, uint32_t caps)
{
    int free_slot = -1;
    int i;

    if (!subject || !*subject || strlen(subject) >= SUBJ_LEN || caps == 0)
        return SCDE_ERR_ARG;
    for (i = 0; i < SCDE_CAP_SUBJECTS; i++) {
        if (!subjects[i].used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (strcmp(subjects[i].name, subject) == 0) {
            subjects[i].caps |= caps;
            return SCDE_OK;
        }
    }
    if (free_slot < 0)
        return SCDE_ERR_FULL;
    subjects[free_slot].used = 1;
    snprintf(subjects[free_slot].name, sizeof(subjects[free_slot].name),
             "%.*s", SUBJ_LEN - 1, subject);
    subjects[free_slot].caps = caps;
    return SCDE_OK;
}

int scde_security_revoke(const char *subject, uint32_t caps)
{
    subject_t *s;

    if (!subject || !*subject)
        return SCDE_ERR_ARG;
    s = find_subject(subject);
    if (!s)
        return SCDE_ERR_NOT_FOUND;
    s->caps &= ~caps;
    return SCDE_OK;
}

int scde_security_has(const char *subject, uint32_t caps)
{
    subject_t *s;

    if (!subject || !*subject || caps == 0)
        return 0;
    s = find_subject(subject);
    return s && (s->caps & caps) == caps;
}

int scde_security_check(const char *subject, uint32_t caps)
{
    if (!subject || !*subject || caps == 0) {
        deny(subject ? subject : "-", caps, "invalid capability request");
        return SCDE_ERR;
    }
    if ((scde_security_has(subject, caps) & 1) == 0) {
        deny(subject, caps, "capability not granted");
        return SCDE_ERR;
    }
    return SCDE_OK;
}

uint32_t scde_capability_from_name(const char *name)
{
    size_t i;

    if (!name)
        return SCDE_CAP_NONE;
    for (i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (strcmp(cap_names[i].name, name) == 0)
            return cap_names[i].cap;
    }
    return SCDE_CAP_NONE;
}

const char *scde_capability_name(uint32_t cap)
{
    size_t i;

    if (cap == SCDE_CAP_NONE)
        return "none";
    for (i = 0; i < sizeof(cap_names) / sizeof(cap_names[0]); i++) {
        if (cap_names[i].cap == cap)
            return cap_names[i].name;
    }
    return "mixed";
}

/*
 * Per-capability enforcement honesty (single bit only).
 * Kernel-enforced = denied by seccomp/nnp/mount/rlimit inside a FULL
 * sandboxed child when the probe says AVAILABLE.  Everything checked
 * only in-process is BYPASSABLE by direct syscall and must be labelled
 * POLICY-LEVEL ONLY.  Unknown/multi-bit inputs are never overstated.
 */
const char *scde_capability_enforcement(uint32_t cap)
{
    scde_security_features_t f;
    int bits = 0;
    uint32_t v = cap;

    if (cap == SCDE_CAP_NONE)
        return "none";
    while (v) {
        bits += (int)(v & 1u);
        v >>= 1;
    }
    if (bits != 1)
        return "mixed";
    scde_security_features(&f);
    switch (cap) {
    case SCDE_CAP_NET:
    case SCDE_CAP_SPAWN:
        /* FULL tier seccomp denies non-unix sockets + clone/fork */
        if (f.seccomp == SCDE_FEATURE_AVAILABLE)
            return "KERNEL-ENFORCED";
        return "POLICY-LEVEL ONLY";
    case SCDE_CAP_FS_READ:
    case SCDE_CAP_FS_WRITE:
    case SCDE_CAP_FS_EXEC:
        /* mount-ns hiding when fs probe passes, else rlimit/seccomp */
        if (f.fs == SCDE_FEATURE_AVAILABLE &&
            f.seccomp == SCDE_FEATURE_AVAILABLE)
            return "KERNEL-ENFORCED";
        if (f.seccomp == SCDE_FEATURE_AVAILABLE ||
            f.no_new_privs == SCDE_FEATURE_AVAILABLE)
            return "LIMITED";
        return "POLICY-LEVEL ONLY";
    case SCDE_CAP_FS_WATCH:
    case SCDE_CAP_CONFIG:
    case SCDE_CAP_AUDIT:
    case SCDE_CAP_DEVICE:
    case SCDE_CAP_PLUGIN_LOAD:
        /* userspace mediation only: bypassable via direct syscall */
        return "BYPASSABLE";
    default:
        return "BYPASSABLE";
    }
}

int scde_security_restrict_self(void)
{
    struct rlimit rl;
    int ok = 1;

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        ok = 0;

    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    if (setrlimit(RLIMIT_CORE, &rl) != 0)
        ok = 0;

    return ok ? SCDE_OK : SCDE_ERR;
}
