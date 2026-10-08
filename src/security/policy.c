/*
 * SCDE Security Layer - security policy and the policy gate.
 *
 * Policies are simple 0/1 switches with safe defaults; everything that is
 * not explicitly allowed is refused (fail-closed) at the gate.
 */
#include <scde/security.h>

#include "security_internal.h"

static const int defaults[SCDE_POL_COUNT] = {
    0,     /* ALLOW_UNTRUSTED_PLUGINS: in-process native code requires
              TRUSTED by default; enabling this is an explicit unsafe
              override (documented in docs/SECURITY.md) */
    1,     /* BLOCK_KNOWN_BAD: known-bad hash => BLOCKED */
    0,     /* DENY_SUSPICIOUS: suspicious runs in the sandbox instead */
    0,     /* REQUIRE_TRUSTED_PLUGINS (legacy; subsumed by the default
              above, kept for compatibility) */
    1      /* AUDIT_FILE_SCANS */
};

static int values[SCDE_POL_COUNT];
static int values_ready;

static void ensure_defaults(void)
{
    int i;

    if (values_ready)
        return;
    for (i = 0; i < SCDE_POL_COUNT; i++)
        values[i] = defaults[i];
    values_ready = 1;
}

int scde_policy_set(int key, int value)
{
    ensure_defaults();
    if (key < 0 || key >= SCDE_POL_COUNT)
        return SCDE_ERR_ARG;
    if (value != 0 && value != 1)
        return SCDE_ERR_ARG;
    values[key] = value;
    return SCDE_OK;
}

int scde_policy_get(int key)
{
    ensure_defaults();
    if (key < 0 || key >= SCDE_POL_COUNT)
        return -1;
    return values[key];
}

int scde_policy_default(int key)
{
    if (key < 0 || key >= SCDE_POL_COUNT)
        return -1;
    return defaults[key];
}

int scde_policy_reset(void)
{
    int i;

    for (i = 0; i < SCDE_POL_COUNT; i++)
        values[i] = defaults[i];
    values_ready = 1;
    return SCDE_OK;
}

int scde_security_allow(const scde_scan_result_t *result)
{
    if (!result)
        return SCDE_ERR_ARG;
    ensure_defaults();

    switch (result->status) {
    case SCDE_THREAT_SAFE:
    case SCDE_THREAT_UNKNOWN:
        return SCDE_OK;
    case SCDE_THREAT_SUSPICIOUS:
        if (values[SCDE_POL_DENY_SUSPICIOUS]) {
            scde__stats()->denied++;
            scde_audit_log(SCDE_AUDIT_POLICY_DENIED, result->detail,
                           result->risk, "suspicious refused by policy");
            return SCDE_ERR;
        }
        return SCDE_OK;
    case SCDE_THREAT_MALICIOUS:
    case SCDE_THREAT_BLOCKED:
        scde__stats()->denied++;
        scde_audit_log(SCDE_AUDIT_POLICY_DENIED, result->detail, result->risk,
                       "malicious refused by policy");
        return SCDE_ERR;
    default:
        return SCDE_ERR;           /* unknown status: fail closed */
    }
}

/*
 * Scanner + trust + policy -> one enforcement decision (fail-closed).
 * Pure decision: no side effects, no auditing (callers audit outcomes).
 * See docs/SECURITY.md for the full mapping table.
 */
int scde_security_decide(const scde_scan_result_t *result,
                         scde_decision_t *out)
{
    scde_security_features_t f;

    if (!out)
        return SCDE_ERR_ARG;
    *out = SCDE_DECIDE_DENY;            /* fail-closed default */
    if (!result)
        return SCDE_ERR_ARG;
    ensure_defaults();

    switch (result->status) {
    case SCDE_THREAT_BLOCKED:
    case SCDE_THREAT_MALICIOUS:
        return SCDE_OK;                 /* known-bad / malicious: DENY */
    case SCDE_THREAT_SUSPICIOUS:
        if (values[SCDE_POL_DENY_SUSPICIOUS])
            return SCDE_OK;             /* DENY */
        if (result->trust == SCDE_TRUST_TRUSTED) {
            *out = SCDE_DECIDE_ALLOW;   /* trusted freedom */
            return SCDE_OK;
        }
        scde_security_features(&f);
        if (f.sandbox == SCDE_FEATURE_UNAVAILABLE)
            return SCDE_OK;             /* cannot isolate: DENY */
        *out = SCDE_DECIDE_SANDBOX;
        return SCDE_OK;
    case SCDE_THREAT_SAFE:
    case SCDE_THREAT_UNKNOWN:
        break;
    default:
        return SCDE_OK;                 /* unknown status: DENY */
    }

    switch (result->trust) {
    case SCDE_TRUST_BLOCKED:
        return SCDE_OK;                 /* DENY */
    case SCDE_TRUST_TRUSTED:
        *out = SCDE_DECIDE_ALLOW;
        return SCDE_OK;
    case SCDE_TRUST_UNTRUSTED:
        scde_security_features(&f);
        if (f.sandbox == SCDE_FEATURE_UNAVAILABLE)
            return SCDE_OK;             /* fail-closed: no sandbox, no run */
        *out = SCDE_DECIDE_SANDBOX;
        return SCDE_OK;
    default:
        /* UNKNOWN trust: restricted execution is the policy default -
         * not "safe", not unrestricted */
        *out = SCDE_DECIDE_RESTRICT;
        return SCDE_OK;
    }
}
