/*
 * SCDE Mirage - environment/process decoy registry.
 *
 * In-memory dummy entries only.  Every probe is denied and audited with
 * pid/uid/process/trust metadata.  ADVISORY detection, never enforcement.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>
#include <scde/security_mirage.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int used;
    int kind;
    char name[SCDE_MIRAGE_NAME_LEN];
    char dummy[96];
} mirage_entry_t;

static int mir_inited;
static int mir_on = 1;
static int mir_probes_total;
static mirage_entry_t table[SCDE_MIRAGE_MAX];

const char *scde_mirage_kind_name(int kind)
{
    switch (kind) {
    case SCDE_MIRAGE_ENV:
        return "env";
    case SCDE_MIRAGE_RESOURCE:
        return "resource";
    case SCDE_MIRAGE_CAPABILITY:
        return "capability";
    case SCDE_MIRAGE_PROCESS:
        return "process";
    default:
        return "unknown";
    }
}

static void proc_comm(char *out, size_t len)
{
    FILE *f;

    if (!out || len == 0)
        return;
    out[0] = '\0';
    f = fopen("/proc/self/comm", "r");
    if (!f)
        return;
    if (fgets(out, (int)len, f) != NULL) {
        size_t n = strlen(out);

        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
        {
            size_t c;

            for (c = 0; out[c]; c++) {
                if (!isprint((unsigned char)out[c]) || out[c] == ' ')
                    out[c] = '?';
                if (c >= 31)
                    break;
            }
            out[31] = '\0';
        }
    }
    fclose(f);
    if (!out[0])
        snprintf(out, len, "unknown");
}

static void audit_with_meta(int event, const char *subject, int risk,
                            const char *op, int trust)
{
    char detail[64];
    char comm[32];
    const char *ts;

    proc_comm(comm, sizeof(comm));
    switch (trust) {
    case SCDE_TRUST_TRUSTED:
        ts = "trusted";
        break;
    case SCDE_TRUST_UNTRUSTED:
        ts = "untrusted";
        break;
    case SCDE_TRUST_BLOCKED:
        ts = "blocked";
        break;
    default:
        ts = "unknown";
        break;
    }
    snprintf(detail, sizeof(detail), "pid=%ld uid=%ld %s op=%s %s",
             (long)getpid(), (long)getuid(), comm, op ? op : "-", ts);
    scde_audit_log(event, subject, risk, detail);
}

static int name_ok(const char *name)
{
    size_t i, n;

    if (!name || !*name)
        return 0;
    n = strlen(name);
    if (n == 0 || n >= SCDE_MIRAGE_NAME_LEN)
        return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            continue;
        return 0;
    }
    return 1;
}

static mirage_entry_t *find_entry(const char *name)
{
    int i;

    for (i = 0; i < SCDE_MIRAGE_MAX; i++) {
        if (table[i].used && strcmp(table[i].name, name) == 0)
            return &table[i];
    }
    return NULL;
}

static int is_escape_op(const char *op)
{
    if (!op || !*op)
        return 0;
    return strcmp(op, "escape") == 0 || strcmp(op, "breakout") == 0 ||
           strcmp(op, "exec") == 0 || strcmp(op, "ptrace") == 0 ||
           strcmp(op, "mount") == 0 || strstr(op, "..") != NULL;
}

static int is_modify_op(const char *op)
{
    if (!op)
        return 0;
    return strcmp(op, "write") == 0 || strcmp(op, "modify") == 0 ||
           strcmp(op, "set") == 0 || strcmp(op, "export") == 0;
}

int scde_mirage_init(void)
{
    if (mir_inited)
        return SCDE_OK;
    memset(table, 0, sizeof(table));
    mir_probes_total = 0;
    mir_on = 1;
    mir_inited = 1;
    return SCDE_OK;
}

void scde_mirage_shutdown(void)
{
    if (!mir_inited)
        return;
    memset(table, 0, sizeof(table));
    mir_probes_total = 0;
    mir_on = 1;
    mir_inited = 0;
}

int scde_mirage_active(void)
{
    return mir_inited;
}

int scde_mirage_enable(void)
{
    if (!mir_inited)
        return SCDE_ERR_STATE;
    mir_on = 1;
    return SCDE_OK;
}

int scde_mirage_disable(void)
{
    if (!mir_inited)
        return SCDE_ERR_STATE;
    mir_on = 0;
    return SCDE_OK;
}

int scde_mirage_enabled(void)
{
    return mir_inited && mir_on;
}

int scde_mirage_create(int kind, const char *name)
{
    mirage_entry_t *e;
    int i;

    if (!mir_inited)
        return SCDE_ERR_STATE;
    if (kind < 0 || kind >= SCDE_MIRAGE_KIND_COUNT)
        return SCDE_ERR_ARG;
    if (!name_ok(name))
        return SCDE_ERR_ARG;
    e = find_entry(name);
    if (e)
        return SCDE_OK;             /* idempotent */
    for (i = 0; i < SCDE_MIRAGE_MAX; i++) {
        if (!table[i].used) {
            e = &table[i];
            break;
        }
    }
    if (!e)
        return SCDE_ERR_FULL;
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->kind = kind;
    snprintf(e->name, sizeof(e->name), "%s", name);
    /* dummy value only: clearly fake, never a real handle or secret */
    snprintf(e->dummy, sizeof(e->dummy), "decoy-%s-no-real-access",
             scde_mirage_kind_name(kind));
    scde_audit_log(SCDE_AUDIT_MIRAGE_CREATED, name, 0, "mirage registered");
    return SCDE_OK;
}

int scde_mirage_create_defaults(void)
{
    static const struct {
        int kind;
        const char *name;
    } defs[] = {
        { SCDE_MIRAGE_ENV, "SCDE_DECOY_SESSION" },
        { SCDE_MIRAGE_RESOURCE, "decoy-gpu0" },
        { SCDE_MIRAGE_CAPABILITY, "decoy-admin-token" },
        { SCDE_MIRAGE_PROCESS, "decoy-keyring-pid" },
    };
    size_t i;
    int rc = SCDE_OK;

    if (!mir_inited)
        return SCDE_ERR_STATE;
    for (i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        if (scde_mirage_create(defs[i].kind, defs[i].name) != SCDE_OK)
            rc = SCDE_ERR;
    }
    return rc;
}

int scde_mirage_probe(const char *name, const char *op, int trust)
{
    return scde_mirage_lookup(name, op, trust, NULL, 0);
}

int scde_mirage_lookup(const char *name, const char *op, int trust,
                        char *out_value, size_t out_len)
{
    mirage_entry_t *e;
    const char *use_op;

    if (!mir_inited)
        return SCDE_ERR_STATE;
    if (!name || !*name)
        return SCDE_ERR_ARG;
    use_op = (op && *op) ? op : "probe";
    if (trust != SCDE_TRUST_TRUSTED && trust != SCDE_TRUST_UNTRUSTED &&
        trust != SCDE_TRUST_BLOCKED && trust != SCDE_TRUST_UNKNOWN)
        trust = SCDE_TRUST_UNKNOWN;

    e = find_entry(name);
    if (!mir_on) {
        /* disabled: still deny, but do not count telemetry */
        if (out_value && out_len > 0)
            out_value[0] = '\0';
        return SCDE_ERR;
    }
    if (!e) {
        /* unknown decoy name: fail-closed, audit as generic probe */
        mir_probes_total++;
        audit_with_meta(SCDE_AUDIT_MIRAGE_PROBED, name, 30, use_op,
                        trust);
        if (out_value && out_len > 0)
            out_value[0] = '\0';
        return SCDE_ERR;
    }
    mir_probes_total++;
    if (is_escape_op(use_op)) {
        audit_with_meta(SCDE_AUDIT_MIRAGE_ESCAPE_ATTEMPT, name, 90,
                        use_op, trust);
    } else if (is_modify_op(use_op)) {
        audit_with_meta(SCDE_AUDIT_MIRAGE_MODIFIED, name, 70, use_op,
                        trust);
    } else if (strcmp(use_op, "probe") == 0 ||
               strcmp(use_op, "stat") == 0 ||
               strcmp(use_op, "list") == 0) {
        audit_with_meta(SCDE_AUDIT_MIRAGE_PROBED, name, 40, use_op,
                        trust);
    } else {
        audit_with_meta(SCDE_AUDIT_MIRAGE_ACCESSED, name, 55, use_op,
                        trust);
    }
    /* dummy value only - never a real resource */
    if (out_value && out_len > 0)
        snprintf(out_value, out_len, "%s", e->dummy);
    return SCDE_ERR;                /* always denied */
}

int scde_mirage_count(void)
{
    int i, n = 0;

    if (!mir_inited)
        return 0;
    for (i = 0; i < SCDE_MIRAGE_MAX; i++) {
        if (table[i].used)
            n++;
    }
    return n;
}

int scde_mirage_probes(void)
{
    return mir_probes_total;
}

int scde_mirage_name(int index, char *out, size_t out_len)
{
    int i, seen = 0;

    if (!mir_inited || !out || out_len == 0)
        return SCDE_ERR_ARG;
    for (i = 0; i < SCDE_MIRAGE_MAX; i++) {
        if (!table[i].used)
            continue;
        if (seen == index) {
            snprintf(out, out_len, "%s", table[i].name);
            return SCDE_OK;
        }
        seen++;
    }
    return SCDE_ERR_NOT_FOUND;
}
