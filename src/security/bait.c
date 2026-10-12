/*
 * SCDE Bait - filesystem decoy subsystem.
 *
 * Dummy-only decoys under one root directory.  Detection is polling
 * (scde_bait_check) plus explicit telemetry (scde_bait_report); both feed
 * the audit log with pid/uid/process/trust metadata.  ADVISORY only.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>
#include <scde/security_bait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BAIT_MARKER "SCDE-DECOY-DUMMY-DO-NOT-USE"
#define DECOY_DIR_MODE 0700

typedef struct {
    int used;
    int kind;
    char id[SCDE_BAIT_ID_LEN];
    char path[SCDE_BAIT_PATH_LEN];
    off_t size;
    time_t mtime;
    mode_t mode;
    int exists;
} bait_entry_t;

static int bait_inited;
static int bait_on = 1;
static int bait_hits_total;
static char bait_root[SCDE_BAIT_PATH_LEN];
static int bait_owned;              /* 1 when we created bait_root */
static bait_entry_t entries[SCDE_BAIT_MAX];

const char *scde_bait_kind_name(int kind)
{
    switch (kind) {
    case SCDE_BAIT_CONFIG:
        return "config";
    case SCDE_BAIT_CACHE:
        return "cache";
    case SCDE_BAIT_DOCUMENT:
        return "document";
    case SCDE_BAIT_EXECUTABLE:
        return "executable";
    case SCDE_BAIT_APPDATA:
        return "appdata";
    default:
        return "unknown";
    }
}

/* ------------------------------------------------------------ helpers -- */

static void proc_comm(char *out, size_t len)
{
    FILE *f;
    int c;

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
        /* keep it shell/log safe */
        for (c = 0; out[c]; c++) {
            if (!isprint((unsigned char)out[c]) || out[c] == ' ')
                out[c] = '?';
            if (c >= 31)
                break;
        }
        out[31] = '\0';
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
    long pid = (long)getpid();
    long uid = (long)getuid();
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
    snprintf(detail, sizeof(detail), "pid=%ld uid=%ld %s op=%s %s", pid,
             uid, comm, op ? op : "-", ts);
    scde_audit_log(event, subject, risk, detail);
}

static int leaf_ok(const char *leaf)
{
    size_t i, n;

    if (!leaf || !*leaf)
        return 0;
    n = strlen(leaf);
    if (n == 0 || n >= 96)
        return 0;
    if (leaf[0] == '.')
        return 0;
    for (i = 0; i < n; i++) {
        char c = leaf[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            continue;
        return 0;
    }
    if (strstr(leaf, "..") != NULL)
        return 0;
    return 1;
}

static int ensure_dir(const char *path)
{
    struct stat st;

    if (lstat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? SCDE_OK : SCDE_ERR;
    if (mkdir(path, DECOY_DIR_MODE) != 0)
        return SCDE_ERR;
    chmod(path, DECOY_DIR_MODE);
    return SCDE_OK;
}

static const char *dummy_for(int kind, const char *leaf)
{
    (void)leaf;
    switch (kind) {
    case SCDE_BAIT_CONFIG:
        return BAIT_MARKER
               "\n# fake SCDE decoy config - no real settings here\n"
               "theme=decoy-grey\nfont=decoy-mono-10\n"
               "server=decoy.invalid\nusername=decoy-user\n"
               "password=DECOY-NOT-A-REAL-PASSWORD\n";
    case SCDE_BAIT_CACHE:
        return BAIT_MARKER
               "\n# fake cache - regenerated dummy bytes\n"
               "blob=AAAAAAAAAAAAAAAA\nblob2=BBBBBBBBBBBBBBBB\n";
    case SCDE_BAIT_DOCUMENT:
        return BAIT_MARKER
               "\nDecoy document for threat detection.\n"
               "There is no real content in this file.\n";
    case SCDE_BAIT_EXECUTABLE:
        return "#!/bin/sh\n# " BAIT_MARKER
               " - harmless decoy, does nothing useful\n"
               "echo decoy\n";
    case SCDE_BAIT_APPDATA:
    default:
        return BAIT_MARKER
               "\n{\n  \"profile\": \"decoy\",\n"
               "  \"token\": \"DECOY-NOT-A-REAL-TOKEN\",\n"
               "  \"cookie\": \"DECOY-NOT-A-REAL-COOKIE\"\n}\n";
    }
}

static mode_t mode_for(int kind)
{
    /* decoys are never executable: exec attempts must fail at the kernel */
    (void)kind;
    return 0600;
}

static bait_entry_t *find_by_path(const char *path)
{
    int i;

    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        if (entries[i].used && strcmp(entries[i].path, path) == 0)
            return &entries[i];
    }
    return NULL;
}

static bait_entry_t *alloc_entry(void)
{
    int i;

    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        if (!entries[i].used)
            return &entries[i];
    }
    return NULL;
}

static int snapshot(bait_entry_t *e)
{
    struct stat st;

    if (lstat(e->path, &st) != 0) {
        e->exists = 0;
        return SCDE_ERR_NOT_FOUND;
    }
    e->exists = 1;
    e->size = st.st_size;
    e->mtime = st.st_mtime;
    e->mode = st.st_mode & 07777;
    return SCDE_OK;
}

/* ------------------------------------------------------------ lifecycle -- */

int scde_bait_init(const char *dir)
{
    char tmp[SCDE_BAIT_PATH_LEN];

    if (bait_inited)
        return SCDE_OK;
    memset(entries, 0, sizeof(entries));
    bait_hits_total = 0;
    bait_on = 1;
    bait_owned = 0;
    bait_root[0] = '\0';

    if (dir && *dir) {
        if (strlen(dir) >= sizeof(bait_root))
            return SCDE_ERR_ARG;
        snprintf(bait_root, sizeof(bait_root), "%s", dir);
        if (ensure_dir(bait_root) != SCDE_OK)
            return SCDE_ERR_IO;
        bait_owned = 0;             /* caller-owned directory */
    } else {
        const char *env = getenv("SCDE_BAIT_DIR");

        if (env && *env) {
            if (strlen(env) >= sizeof(bait_root))
                return SCDE_ERR_ARG;
            snprintf(bait_root, sizeof(bait_root), "%s", env);
            if (ensure_dir(bait_root) != SCDE_OK)
                return SCDE_ERR_IO;
            bait_owned = 0;
        } else {
            snprintf(tmp, sizeof(tmp), "/tmp/scde-bait-XXXXXX");
            if (mkdtemp(tmp) == NULL)
                return SCDE_ERR_IO;
            snprintf(bait_root, sizeof(bait_root), "%s", tmp);
            bait_owned = 1;
        }
    }
    /* root itself is locked down: decoys must not leak outward */
    chmod(bait_root, DECOY_DIR_MODE);
    bait_inited = 1;
    return SCDE_OK;
}

void scde_bait_shutdown(void)
{
    int i;

    if (!bait_inited)
        return;
    if (bait_owned && bait_root[0]) {
        /* remove only files we track + subdirs we made; bounded */
        for (i = 0; i < SCDE_BAIT_MAX; i++) {
            if (entries[i].used)
                unlink(entries[i].path);
        }
        {
            static const char *subs[] = {
                "config", "cache", "document", "executable", "appdata",
                NULL
            };
            int k;

            for (k = 0; subs[k]; k++) {
                char sub[SCDE_BAIT_PATH_LEN];
                size_t rl = strlen(bait_root);
                size_t sl = strlen(subs[k]);

                if (rl + 1 + sl + 1 > sizeof(sub))
                    continue;
                memcpy(sub, bait_root, rl);
                sub[rl] = '/';
                memcpy(sub + rl + 1, subs[k], sl + 1);
                rmdir(sub);
            }
        }
        rmdir(bait_root);
    }
    memset(entries, 0, sizeof(entries));
    bait_root[0] = '\0';
    bait_owned = 0;
    bait_inited = 0;
    bait_on = 1;
}

int scde_bait_active(void)
{
    return bait_inited;
}

const char *scde_bait_dir(void)
{
    return bait_inited ? bait_root : "";
}

int scde_bait_enable(void)
{
    if (!bait_inited)
        return SCDE_ERR_STATE;
    bait_on = 1;
    return SCDE_OK;
}

int scde_bait_disable(void)
{
    if (!bait_inited)
        return SCDE_ERR_STATE;
    bait_on = 0;
    return SCDE_OK;
}

int scde_bait_enabled(void)
{
    return bait_inited && bait_on;
}

/* -------------------------------------------------------------- create -- */

int scde_bait_create(int kind, const char *leaf)
{
    char sub[SCDE_BAIT_PATH_LEN];
    char full[SCDE_BAIT_PATH_LEN];
    bait_entry_t *e;
    const char *body;
    int fd;
    size_t left, done = 0, body_len;

    if (!bait_inited)
        return SCDE_ERR_STATE;
    if (kind < 0 || kind >= SCDE_BAIT_KIND_COUNT)
        return SCDE_ERR_ARG;
    if (!leaf_ok(leaf))
        return SCDE_ERR_ARG;

    {
        const char *kname = scde_bait_kind_name(kind);
        size_t rl = strlen(bait_root);
        size_t kl = strlen(kname);
        size_t ll = strlen(leaf);
        size_t sl;

        if (rl + 1 + kl + 1 > sizeof(sub))
            return SCDE_ERR_ARG;
        memcpy(sub, bait_root, rl);
        sub[rl] = '/';
        memcpy(sub + rl + 1, kname, kl + 1);
        if (ensure_dir(sub) != SCDE_OK)
            return SCDE_ERR_IO;
        sl = strlen(sub);
        if (sl + 1 + ll + 1 > sizeof(full))
            return SCDE_ERR_ARG;
        memcpy(full, sub, sl);
        full[sl] = '/';
        memcpy(full + sl + 1, leaf, ll + 1);
    }

    e = find_by_path(full);
    if (!e) {
        e = alloc_entry();
        if (!e)
            return SCDE_ERR_FULL;
    }
    body = dummy_for(kind, leaf);
    body_len = strlen(body);

    fd = open(full, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return SCDE_ERR_IO;
    left = body_len;
    while (left > 0) {
        ssize_t w = write(fd, body + done, left);

        if (w <= 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(full);
            return SCDE_ERR_IO;
        }
        done += (size_t)w;
        left -= (size_t)w;
    }
    /* fchmod on the descriptor we just wrote, never chmod() on the path:
       the file cannot be swapped for a link underneath an open fd */
    fchmod(fd, mode_for(kind));     /* never executable */
    close(fd);

    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->kind = kind;
    snprintf(e->id, sizeof(e->id), "%s/%s", scde_bait_kind_name(kind),
             leaf);
    snprintf(e->path, sizeof(e->path), "%s", full);
    snapshot(e);

    audit_with_meta(SCDE_AUDIT_BAIT_CREATED, e->id, 0, "create",
                    SCDE_TRUST_UNKNOWN);
    return SCDE_OK;
}

int scde_bait_create_defaults(void)
{
    static const struct {
        int kind;
        const char *leaf;
    } defs[] = {
        { SCDE_BAIT_CONFIG, "decoy-settings.conf" },
        { SCDE_BAIT_CACHE, "decoy-thumb.cache" },
        { SCDE_BAIT_DOCUMENT, "decoy-notes.txt" },
        { SCDE_BAIT_EXECUTABLE, "decoy-helper.sh" },
        { SCDE_BAIT_APPDATA, "decoy-profile.json" },
    };
    size_t i;
    int rc = SCDE_OK;

    if (!bait_inited)
        return SCDE_ERR_STATE;
    for (i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        int st = scde_bait_create(defs[i].kind, defs[i].leaf);

        if (st != SCDE_OK)
            rc = st;
    }
    return rc;
}

/* -------------------------------------------------------------- detect -- */

int scde_bait_report(const char *decoy_path, const char *op, int trust)
{
    bait_entry_t *e;
    char id[SCDE_BAIT_ID_LEN];
    int exec_like = 0;

    if (!bait_inited)
        return SCDE_ERR_STATE;
    if (!decoy_path || !*decoy_path || !op || !*op)
        return SCDE_ERR_ARG;
    if (trust != SCDE_TRUST_TRUSTED && trust != SCDE_TRUST_UNTRUSTED &&
        trust != SCDE_TRUST_BLOCKED && trust != SCDE_TRUST_UNKNOWN)
        trust = SCDE_TRUST_UNKNOWN;

    e = find_by_path(decoy_path);
    if (e)
        snprintf(id, sizeof(id), "%s", e->id);
    else
        snprintf(id, sizeof(id), "%.*s", (int)sizeof(id) - 1, decoy_path);

    exec_like = (strcmp(op, "exec") == 0 || strcmp(op, "execute") == 0 ||
                 strcmp(op, "spawn") == 0);
    if (exec_like) {
        bait_hits_total++;
        audit_with_meta(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT, id, 85, op,
                        trust);
        return SCDE_OK;
    }
    if (!bait_on)
        return SCDE_OK;             /* disabled: inert, still harmless */
    bait_hits_total++;
    audit_with_meta(SCDE_AUDIT_BAIT_ACCESSED, id, 60, op, trust);
    return SCDE_OK;
}

int scde_bait_check(void)
{
    int i, new_hits = 0;
    struct stat st;

    if (!bait_inited)
        return SCDE_ERR_STATE;
    if (!bait_on)
        return 0;
    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        bait_entry_t *e = &entries[i];

        if (!e->used)
            continue;
        if (lstat(e->path, &st) != 0) {
            /* deleted (or moved) decoy: report once, keep the slot so
             * re-creation is still detected as MODIFIED later */
            if (e->exists) {
                e->exists = 0;
                bait_hits_total++;
                new_hits++;
                audit_with_meta(SCDE_AUDIT_BAIT_DELETED, e->id, 70,
                                "unlink", SCDE_TRUST_UNKNOWN);
            }
            continue;
        }
        if (!e->exists) {
            e->exists = 1;
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            e->mode = st.st_mode & 07777;
            bait_hits_total++;
            new_hits++;
            audit_with_meta(SCDE_AUDIT_BAIT_MODIFIED, e->id, 60,
                            "recreated", SCDE_TRUST_UNKNOWN);
            continue;
        }
        /* executable bit appeared: someone wants to run the decoy.
         * Never chmod through a symlink: an attacker-swapped link must not
         * let us change the mode of an arbitrary target file. */
        if ((st.st_mode & 0111) != 0 && !S_ISLNK(st.st_mode)) {
            int wfd = open(e->path, O_RDONLY | O_NOFOLLOW);

            bait_hits_total++;
            new_hits++;
            audit_with_meta(SCDE_AUDIT_BAIT_EXECUTION_ATTEMPT, e->id,
                            85, "exec-bit", SCDE_TRUST_UNKNOWN);
            if (wfd >= 0) {
                fchmod(wfd, mode_for(e->kind));  /* back to inert */
                close(wfd);
            }
            e->mode = mode_for(e->kind);
        } else if (S_ISLNK(st.st_mode)) {
            /* a link where a decoy should be: report, never touch it */
            bait_hits_total++;
            new_hits++;
            audit_with_meta(SCDE_AUDIT_BAIT_MODIFIED, e->id, 60,
                            "replaced-by-symlink", SCDE_TRUST_UNKNOWN);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            e->mode = st.st_mode & 07777;
        } else if (st.st_size != e->size || st.st_mtime != e->mtime) {
            e->size = st.st_size;
            e->mtime = st.st_mtime;
            e->mode = st.st_mode & 07777;
            bait_hits_total++;
            new_hits++;
            audit_with_meta(SCDE_AUDIT_BAIT_MODIFIED, e->id, 60,
                            "modified", SCDE_TRUST_UNKNOWN);
        }
        /* unreadable decoy (someone chmod 000 it): permission telemetry */
        if (access(e->path, R_OK) != 0) {
            bait_hits_total++;
            new_hits++;
            audit_with_meta(SCDE_AUDIT_BAIT_PERMISSION_DENIED, e->id,
                            50, "perm", SCDE_TRUST_UNKNOWN);
        }
    }
    return new_hits;
}

/* -------------------------------------------------------------- inspect -- */

int scde_bait_count(void)
{
    int i, n = 0;

    if (!bait_inited)
        return 0;
    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        if (entries[i].used)
            n++;
    }
    return n;
}

int scde_bait_hits(void)
{
    return bait_hits_total;
}

int scde_bait_path(int index, char *out, size_t out_len)
{
    int i, seen = 0;

    if (!bait_inited || !out || out_len == 0)
        return SCDE_ERR_ARG;
    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        if (!entries[i].used)
            continue;
        if (seen == index) {
            snprintf(out, out_len, "%s", entries[i].path);
            return SCDE_OK;
        }
        seen++;
    }
    return SCDE_ERR_NOT_FOUND;
}

/*
 * Dummy hygiene: every decoy must carry the decoy marker and must not
 * contain obvious real-credential material.  Bounded read (first 4 KiB).
 */
int scde_bait_verify_dummy(void)
{
    static const char *const banned[] = {
        "BEGIN RSA PRIVATE KEY",
        "BEGIN OPENSSH PRIVATE KEY",
        "AKIA",                    /* AWS key id prefix */
        "ghp_",                    /* GitHub token prefix */
        "xoxb-",                   /* Slack token prefix */
        NULL
    };
    int i;

    if (!bait_inited)
        return SCDE_ERR_STATE;
    for (i = 0; i < SCDE_BAIT_MAX; i++) {
        bait_entry_t *e = &entries[i];
        char buf[4096];
        int fd;
        ssize_t n;
        int k;

        if (!e->used)
            continue;
        fd = open(e->path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0)
            continue;               /* deleted decoy: check() reports it */
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            return SCDE_ERR;
        buf[n] = '\0';
        if (strstr(buf, BAIT_MARKER) == NULL)
            return SCDE_ERR;
        for (k = 0; banned[k]; k++) {
            if (strstr(buf, banned[k]) != NULL)
                return SCDE_ERR;
        }
        /* symlinks must never point at the real filesystem */
        {
            struct stat st;

            if (lstat(e->path, &st) == 0 && S_ISLNK(st.st_mode))
                return SCDE_ERR;
        }
    }
    return SCDE_OK;
}
