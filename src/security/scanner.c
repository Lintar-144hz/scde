/*
 * SCDE Security Layer - built-in malware scanner.
 *
 * Pipeline (never executes the analysed file):
 *
 *   path check -> lstat/symlink -> cache -> metadata -> type (magic)
 *      -> SHA-256 vs known-bad/known-goods -> ELF or script static rules
 *      -> risk score -> status -> audit -> cache store
 *
 * "Quick inspection first, deep scan only when needed": content is only
 * read for small text files and ELF headers, executables are bounded, and
 * files above 4 MiB are metadata-only.  ALWAYS_ON adds an inotify watcher
 * that only looks at newly created/modified executables and scripts - it
 * never walks the filesystem.
 *
 * Scoring: 0-19 SAFE, 20-49 UNKNOWN, 50-74 SUSPICIOUS, 75-100 MALICIOUS,
 * BLOCKED when a known-bad hash hits and policy says so.  Heuristic groups
 * are capped per category and a single heuristic group can never reach
 * MALICIOUS - several independent indicators are required.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>

#include "security_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_SIZE_MAX   (4u * 1024u * 1024u)  /* hashable / analysable */
#define DEEP_SIZE_MAX   (512u * 1024u)        /* content rule window */
#define MAX_RULES       64
#define MAX_HASHES      128
#define SUB_LEN         96

/* ------------------------------------------------------------- SHA-256 -- */

typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char buf[64];
    size_t fill;
} sha256_ctx;

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a, b, d, e, f, g, hh, cc;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    hh = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + s1 + ch + k256[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;

        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += hh;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    memcpy(c->h, iv, sizeof(iv));
    c->bits = 0;
    c->fill = 0;
}

/* raw buffer fill - does not touch the bit counter (used by sha256_final) */
static void sha256_absorb(sha256_ctx *c, const unsigned char *p, size_t n)
{
    while (n > 0) {
        size_t take = 64 - c->fill;

        if (take > n)
            take = n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill == 64) {
            sha256_block(c, c->buf);
            c->fill = 0;
        }
    }
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, size_t n)
{
    c->bits += (uint64_t)n * 8u;
    sha256_absorb(c, p, n);
}

static void sha256_final(sha256_ctx *c, char out_hex[65])
{
    static const char hexd[] = "0123456789abcdef";
    unsigned char pad[128];
    unsigned char tail[8];
    uint64_t bits = c->bits;
    size_t pad_len;
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80u;
    pad_len = (c->fill < 56) ? (56 - c->fill) : (120 - c->fill);
    sha256_absorb(c, pad, pad_len);
    for (i = 0; i < 8; i++)
        tail[i] = (unsigned char)((bits >> (56 - 8 * i)) & 0xffu);
    sha256_absorb(c, tail, 8);

    /* 8 state words x 4 bytes = 32 bytes = 64 hex chars */
    for (i = 0; i < 32; i++) {
        unsigned char byte = (unsigned char)((c->h[i >> 2] >>
                                              (24 - 8 * (i & 3))) & 0xffu);

        out_hex[i * 2] = hexd[byte >> 4];
        out_hex[i * 2 + 1] = hexd[byte & 0x0f];
    }
    out_hex[64] = '\0';
}

/* --------------------------------------------------------- scanner core -- */

typedef struct {
    int weight;
    int reason;                     /* scde_reason_t */
    int group;
    char sub[SUB_LEN];
} rule_t;

typedef struct {
    char hex[65];
    char label[40];
} hash_entry_t;

typedef struct {
    int used;
    char path[SCDE_PATH_MAX];
    off_t size;
    int64_t mtime;
    scde_scan_result_t res;
} cache_entry_t;

static int scan_mode = SCDE_SCAN_PASSIVE;
static int scanner_ready;

static rule_t db_rules[MAX_RULES];
static int n_db_rules;
static hash_entry_t bad_hashes[MAX_HASHES];
static int n_bad_hashes;
static hash_entry_t trust_hashes[MAX_HASHES];
static int n_trust_hashes;

static cache_entry_t cache[SCDE_SCAN_RING];
static int cache_next;

/* watch */
static int ifd = -1;
static int n_watch;
static char watch_dirs[SCDE_WATCH_MAX][192];
static int watch_wd[SCDE_WATCH_MAX];

/* groups: separate heuristic buckets so several independent indicators are
   needed before MALICIOUS is possible */
enum {
    G_PATH = 0, G_PERM, G_NAME, G_TYPE, G_ELF, G_DSTRUCT, G_PRIV, G_CRED,
    G_PERSIST, G_REMOTE, G_OBFUSC, G_SYMLINK, G_COUNT
};
#define G_BIT(g) (1u << (g))

#define GROUP_CAP 45                /* max contribution of one group */

typedef struct {
    int gsum[G_COUNT];              /* total weight in the group */
    int gw[G_COUNT];                /* strongest single weight */
    int greason[G_COUNT];           /* reason of that strongest weight */
    int sig_risk;                   /* rules.conf signature weight */
    int sig_reason;
    int definitive;                 /* hash / strong signature */
    int deep;
    uint32_t mask;
} acc_t;

static void acc_add(acc_t *a, int group, int weight, int reason)
{
    if (group < 0 || group >= G_COUNT || weight <= 0)
        return;
    a->gsum[group] += weight;
    if (weight >= a->gw[group]) {
        a->gw[group] = weight;
        a->greason[group] = reason;
    }
    a->mask |= G_BIT(group);
}

/* reason chosen during fold: strongest single contribution */
static int fold_risk(const acc_t *a, int *best_reason)
{
    int g;
    int total = a->sig_risk;
    int best_w = 0;

    *best_reason = SCDE_REASON_NONE;
    if (a->sig_risk > 0) {
        *best_reason = a->sig_reason ? a->sig_reason : SCDE_REASON_SIGNATURE;
        best_w = a->sig_risk;
    }
    for (g = 0; g < G_COUNT; g++) {
        int w = a->gsum[g];

        if (w > GROUP_CAP)
            w = GROUP_CAP;
        total += w;
        if (a->gsum[g] > 0 && w > best_w) {
            best_w = w;
            *best_reason = a->greason[g];
        }
    }
    if (total > 100)
        total = 100;
    if (total < 0)
        total = 0;
    return total;
}

static int popcount32(uint32_t v)
{
    int n = 0;

    while (v) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

/* ------------------------------------------------------- built-in rules -- */

static const struct {
    const char *needle;
    int weight;
    int reason;
    int group;
} builtin_rules[] = {
    /* destructive */
    { "rm -rf /", 45, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { "rm -fr /", 45, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { "mkfs", 40, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { "dd of=/dev/", 40, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { ":(){:|:&};:", 50, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { "shred -", 35, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    { "wipefs", 35, SCDE_REASON_SCRIPT_DESTRUCTIVE, G_DSTRUCT },
    /* privilege escalation */
    { "chmod u+s", 40, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { "chmod +s", 40, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { "chown root", 35, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { "/etc/sudoers", 40, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { ">> /etc/passwd", 45, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { "setcap cap_", 40, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    { "pkexec", 35, SCDE_REASON_SCRIPT_PRIV, G_PRIV },
    /* credential harvesting */
    { "/etc/shadow", 45, SCDE_REASON_SCRIPT_CRED, G_CRED },
    { "id_rsa", 45, SCDE_REASON_SCRIPT_CRED, G_CRED },
    { "id_ed25519", 45, SCDE_REASON_SCRIPT_CRED, G_CRED },
    { ".aws/credentials", 45, SCDE_REASON_SCRIPT_CRED, G_CRED },
    { "history >", 35, SCDE_REASON_SCRIPT_CRED, G_CRED },
    /* persistence */
    { "crontab -", 35, SCDE_REASON_SCRIPT_PERSIST, G_PERSIST },
    { "@reboot", 35, SCDE_REASON_SCRIPT_PERSIST, G_PERSIST },
    { "systemctl enable", 35, SCDE_REASON_SCRIPT_PERSIST, G_PERSIST },
    { ".config/autostart", 35, SCDE_REASON_SCRIPT_PERSIST, G_PERSIST },
    { ".bashrc", 25, SCDE_REASON_SCRIPT_PERSIST, G_PERSIST },
    /* remote execution / download-and-execute */
    { "bash -i >& /dev/tcp", 50, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "/dev/tcp/", 45, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "nc -e", 45, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "curl -fsSL", 40, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "wget -qO-", 40, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "| sh", 35, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    { "| bash", 35, SCDE_REASON_SCRIPT_REMOTE, G_REMOTE },
    /* obfuscated payload */
    { "base64 -d", 35, SCDE_REASON_SCRIPT_OBFUSC, G_OBFUSC },
    { "base64 --decode", 35, SCDE_REASON_SCRIPT_OBFUSC, G_OBFUSC },
    { "openssl enc -d", 35, SCDE_REASON_SCRIPT_OBFUSC, G_OBFUSC },
    { "eval ", 30, SCDE_REASON_SCRIPT_OBFUSC, G_OBFUSC }
};

static int contains_ci(const char *hay, size_t hay_len, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || hay_len < nlen)
        return 0;
    for (i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;

        while (j < nlen) {
            char a = hay[i + j];
            char b = needle[j];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
            j++;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

static int reason_group(int reason, int *group)
{
    switch (reason) {
    case SCDE_REASON_PATH: *group = G_PATH; return 1;
    case SCDE_REASON_PERM: *group = G_PERM; return 1;
    case SCDE_REASON_NAME: *group = G_NAME; return 1;
    case SCDE_REASON_TYPE: *group = G_TYPE; return 1;
    case SCDE_REASON_ELF: *group = G_ELF; return 1;
    case SCDE_REASON_SCRIPT_DESTRUCTIVE: *group = G_DSTRUCT; return 1;
    case SCDE_REASON_SCRIPT_PRIV: *group = G_PRIV; return 1;
    case SCDE_REASON_SCRIPT_CRED: *group = G_CRED; return 1;
    case SCDE_REASON_SCRIPT_PERSIST: *group = G_PERSIST; return 1;
    case SCDE_REASON_SCRIPT_REMOTE: *group = G_REMOTE; return 1;
    case SCDE_REASON_SCRIPT_OBFUSC: *group = G_OBFUSC; return 1;
    case SCDE_REASON_SYMLINK: *group = G_SYMLINK; return 1;
    default: *group = -1; return 0;
    }
}

/* -------------------------------------------------------------- helpers -- */

static int suffix_ci(const char *name, const char *ext)
{
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    size_t i;

    if (nl < el)
        return 0;
    for (i = 0; i < el; i++) {
        char a = name[nl - el + i];
        char b = ext[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static int ext_is_exec(const char *name)
{
    static const char *exts[] = {
        ".so", ".sh", ".bash", ".zsh", ".py", ".js", ".mjs", ".pl", ".lua",
        ".rb", ".bin", ".run", ".out", ".exe", ".elf", NULL
    };
    int i;

    for (i = 0; exts[i]; i++) {
        if (suffix_ci(name, exts[i]))
            return 1;
    }
    return 0;
}

/* case-insensitive equality of the byte range [s, end) with ext */
static int ext_ci(const char *s, const char *end, const char *ext)
{
    size_t n = strlen(ext);
    size_t i;

    if ((size_t)(end - s) != n)
        return 0;
    for (i = 0; i < n; i++) {
        char a = s[i];
        char b = ext[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return 1;
}

static int double_extension(const char *path)
{
    static const char *decoys[] = {
        ".pdf", ".doc", ".docx", ".txt", ".jpg", ".png", ".xls", ".xlsx",
        ".zip", ".html", ".htm", ".rtf", ".csv", NULL
    };
    static const char *payloads[] = {
        ".exe", ".scr", ".bat", ".cmd", ".js", ".vbs", ".ps1", ".sh",
        ".py", ".run", ".bin", ".com", NULL
    };
    const char *base = strrchr(path, '/');
    const char *last;
    const char *prev = NULL;
    int i;

    base = base ? base + 1 : path;
    last = strrchr(base, '.');
    if (!last || last == base || !last[1])
        return 0;
    for (i = (int)(last - base) - 1; i >= 0; i--) {
        if (base[i] == '.') {
            prev = base + i;
            break;
        }
    }
    if (!prev || prev + 1 == last)
        return 0;
    /* "report.pdf.exe": decoy document extension in front of a payload */
    for (i = 0; decoys[i]; i++) {
        int j;

        if (!ext_ci(prev + 1, last, decoys[i] + 1))
            continue;
        for (j = 0; payloads[j]; j++)
            if (ext_ci(last + 1, base + strlen(base), payloads[j] + 1))
                return 1;
        return 0;
    }
    return 0;
}

static int path_group_for(const char *path, int exec)
{
    if (!exec)
        return 0;
    if (strncmp(path, "/tmp/", 5) == 0 || strcmp(path, "/tmp") == 0 ||
        strncmp(path, "/var/tmp/", 9) == 0 ||
        strncmp(path, "/dev/shm/", 9) == 0)
        return 1;
    return 0;
}

static int sensitive_target(const char *resolved)
{
    static const char *files[] = {
        "/etc/passwd", "/etc/shadow", "/etc/sudoers", "/proc/self/mem", NULL
    };
    static const char *dirs[] = {
        "/etc/ssh/", "/root/", "/boot/", NULL
    };
    size_t rl = strlen(resolved);
    int i;

    for (i = 0; files[i]; i++) {
        size_t fl = strlen(files[i]);

        if (rl >= fl && strcmp(resolved + rl - fl, files[i]) == 0)
            return 1;
    }
    for (i = 0; dirs[i]; i++)
        if (strncmp(resolved, dirs[i], strlen(dirs[i])) == 0)
            return 1;
    return 0;
}

/* --------------------------------------------------------- file reading -- */

static int read_at(int fd, off_t off, void *buf, size_t n)
{
    ssize_t r = pread(fd, buf, n, off);

    return r == (ssize_t)n;
}

static int looks_text(const unsigned char *p, size_t n)
{
    size_t i;
    size_t good = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = p[i];

        if (c == '\0')
            return 0;
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7f))
            good++;
    }
    return good * 10 >= n * 9;
}

/* ------------------------------------------------------------ ELF scan -- */

#if defined(__x86_64__)
#define HOST_EM 62
#elif defined(__i386__)
#define HOST_EM 3
#elif defined(__aarch64__)
#define HOST_EM 183
#elif defined(__arm__)
#define HOST_EM 40
#elif defined(__riscv) && __riscv_xlen == 64
#define HOST_EM 243
#else
#define HOST_EM 0
#endif

static uint16_t rd16(const unsigned char *p, int big)
{
    if (big)
        return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint32_t rd32(const unsigned char *p, int big)
{
    if (big)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static uint64_t rd64(const unsigned char *p, int big)
{
    if (big)
        return ((uint64_t)rd32(p, 1) << 32) | rd32(p + 4, 1);
    return ((uint64_t)rd32(p + 4, 0) << 32) | rd32(p, 0);
}

static void elf_scan(int fd, off_t size, const unsigned char *hdr, acc_t *a,
                     const char *path, int exec_bit)
{
    unsigned char ph[64 * 56];
    uint64_t phoff;
    uint16_t phnum, phent, machine;
    int big;
    int i;
    int is64;
    uint64_t strtab_vaddr = 0;
    int have_strtab = 0;
    uint64_t interp_off = 0, interp_len = 0;

    if (size < 64) {
        acc_add(a, G_ELF, 55, SCDE_REASON_ELF);
        return;
    }
    is64 = hdr[4] == 2;
    big = hdr[5] == 2;
    if (hdr[4] != 1 && hdr[4] != 2) {
        acc_add(a, G_ELF, 40, SCDE_REASON_ELF);
        return;
    }
    if (hdr[5] != 1 && hdr[5] != 2) {
        acc_add(a, G_ELF, 40, SCDE_REASON_ELF);
        return;
    }
    machine = rd16(hdr + 18, big);
    if (HOST_EM != 0 && machine != (uint16_t)HOST_EM)
        acc_add(a, G_ELF, 20, SCDE_REASON_ELF);

    {
        uint32_t etype = rd16(hdr + 16, big);

        if (etype == 1)
            acc_add(a, G_ELF, 30, SCDE_REASON_ELF);      /* relocatable */
        else if (etype != 2 && etype != 3)
            acc_add(a, G_ELF, 40, SCDE_REASON_ELF);      /* not executable */
        else if (etype == 2 && !exec_bit)
            acc_add(a, G_PERM, 15, SCDE_REASON_PERM);    /* no exec bit */
    }

    phoff = is64 ? rd64(hdr + 32, big) : (uint64_t)rd32(hdr + 28, big);
    phent = rd16(hdr + (is64 ? 54 : 42), big);
    phnum = rd16(hdr + (is64 ? 56 : 44), big);

    if (phent < (is64 ? 56u : 32u) || phnum > 96 ||
        phoff + (uint64_t)phnum * phent > (uint64_t)size) {
        acc_add(a, G_ELF, 35, SCDE_REASON_ELF);
        return;
    }
    if (phnum == 0)
        return;
    if (phnum > 64)
        phnum = 64;
    if ((size_t)phnum * phent > sizeof(ph)) {
        if (phent > 0)
            phnum = (uint16_t)(sizeof(ph) / phent);
        else
            phnum = 0;
    }
    if (phnum == 0 || !read_at(fd, (off_t)phoff, ph, (size_t)phnum * phent)) {
        acc_add(a, G_ELF, 35, SCDE_REASON_ELF);
        return;
    }

    for (i = 0; i < (int)phnum; i++) {
        const unsigned char *e = ph + (size_t)i * phent;
        uint32_t type = rd32(e, big);
        uint64_t off, filesz;

        if (is64) {
            off = rd64(e + 8, big);
            filesz = rd64(e + 32, big);
        } else {
            off = rd32(e + 4, big);
            filesz = rd32(e + 16, big);
        }
        if (type == 3) {              /* PT_INTERP */
            interp_off = off;
            interp_len = filesz;
        } else if (type == 2) {       /* PT_DYNAMIC */
            if (filesz > 0 && filesz <= 4096 && off + filesz <=
                (uint64_t)size) {
                unsigned char dyn[2048];
                size_t dn = (size_t)filesz;
                size_t j;

                if (dn > sizeof(dyn))
                    dn = sizeof(dyn);
                if (read_at(fd, (off_t)off, dyn, dn)) {
                    for (j = 0; j + (is64 ? 16u : 8u) <= dn;
                         j += (is64 ? 16u : 8u)) {
                        uint64_t tag = is64 ? rd64(dyn + j, big)
                                            : (uint64_t)rd32(dyn + j, big);
                        uint64_t val = is64 ? rd64(dyn + j + 8, big)
                                            : (uint64_t)rd32(dyn + j + 4, big);

                        if (tag == 0)
                            break;
                        if (tag == 5) {           /* DT_STRTAB */
                            strtab_vaddr = val;
                            have_strtab = 1;
                        }
                    }
                }
            }
        } else if (type == 1 && have_strtab) {     /* PT_LOAD, fill later */
            /* mapping handled below via re-scan */
        }
    }

    if (interp_len > 0 && interp_len < 256 &&
        interp_off + interp_len <= (uint64_t)size) {
        char interp[256];

       	if (read_at(fd, (off_t)interp_off, interp, (size_t)interp_len)) {
            size_t n = (size_t)interp_len;

            while (n > 0 && interp[n - 1] == '\0')
                n--;
            interp[n] = '\0';
            if (strncmp(interp, "/tmp/", 5) == 0 ||
                strncmp(interp, "/var/tmp/", 9) == 0 ||
                strncmp(interp, "/dev/shm/", 9) == 0 ||
                strstr(interp, "..") != NULL) {
                acc_add(a, G_ELF, 35, SCDE_REASON_ELF);
            } else if (strncmp(interp, "/lib", 4) != 0 &&
                       strncmp(interp, "/usr/lib", 8) != 0) {
                acc_add(a, G_ELF, 25, SCDE_REASON_ELF);
            }
        }
    }

    /* DT_NEEDED sonames: only if we can map the string table safely */
    if (have_strtab && strtab_vaddr != 0) {
        uint64_t strtab_off = 0;
        int mapped = 0;

        for (i = 0; i < (int)phnum && !mapped; i++) {
            const unsigned char *e = ph + (size_t)i * phent;
            uint32_t type = rd32(e, big);
            uint64_t off, filesz, vaddr;

            if (type != 1)
                continue;
            if (is64) {
                off = rd64(e + 8, big);
                vaddr = rd64(e + 16, big);
                filesz = rd64(e + 32, big);
            } else {
                off = rd32(e + 4, big);
                vaddr = rd32(e + 8, big);
                filesz = rd32(e + 16, big);
            }
            if (strtab_vaddr >= vaddr && strtab_vaddr < vaddr + filesz) {
                strtab_off = off + (strtab_vaddr - vaddr);
                mapped = 1;
            }
        }
        if (mapped) {
            unsigned char dyn[2048];
            size_t dn = 0;
            size_t j;
            int needed = 0;

            /* re-read PT_DYNAMIC contents */
            for (i = 0; i < (int)phnum; i++) {
                const unsigned char *e = ph + (size_t)i * phent;
                uint32_t type = rd32(e, big);
                uint64_t off, filesz;

                if (type != 2)
                    continue;
                if (is64) {
                    off = rd64(e + 8, big);
                    filesz = rd64(e + 32, big);
                } else {
                    off = rd32(e + 4, big);
                    filesz = rd32(e + 16, big);
                }
                dn = (size_t)filesz;
                if (dn > sizeof(dyn))
                    dn = sizeof(dyn);
                if (off + dn <= (uint64_t)size)
                    read_at(fd, (off_t)off, dyn, dn);
                break;
            }
            for (j = 0; j + (is64 ? 16u : 8u) <= dn && needed < 8;
                 j += (is64 ? 16u : 8u)) {
                uint64_t tag = is64 ? rd64(dyn + j, big)
                                    : (uint64_t)rd32(dyn + j, big);
                uint64_t val = is64 ? rd64(dyn + j + 8, big)
                                    : (uint64_t)rd32(dyn + j + 4, big);
                char soname[96];

                if (tag == 0)
                    break;
                if (tag != 1)          /* DT_NEEDED */
                    continue;
                needed++;
                if (strtab_off + val + 8 >= (uint64_t)size)
                    continue;
                if (read_at(fd, (off_t)(strtab_off + val), soname,
                            sizeof(soname) - 1)) {
                    soname[sizeof(soname) - 1] = '\0';
                    if (strchr(soname, '/')) {
                        acc_add(a, G_ELF, 25, SCDE_REASON_ELF);
                        break;
                    }
                }
            }
        }
    }
    (void)path;
}

/* --------------------------------------------------------- script scan -- */

static void script_scan(const unsigned char *buf, size_t n, acc_t *a,
                        char *detail, size_t detail_len)
{
    size_t i;
    int hits = 0;

    for (i = 0; i < sizeof(builtin_rules) / sizeof(builtin_rules[0]); i++) {
        if (contains_ci((const char *)buf, n, builtin_rules[i].needle)) {
            acc_add(a, builtin_rules[i].group, builtin_rules[i].weight,
                    builtin_rules[i].reason);
            if (hits == 0)
                snprintf(detail, detail_len, "rule: %s",
                         builtin_rules[i].needle);
            hits++;
        }
    }
    for (i = 0; i < (size_t)n_db_rules; i++) {
        if (contains_ci((const char *)buf, n, db_rules[i].sub)) {
            a->sig_risk += db_rules[i].weight;
            a->sig_reason = db_rules[i].reason;
            if (db_rules[i].weight >= 75)
                a->definitive = 1;
            if (hits == 0)
                snprintf(detail, detail_len, "signature: %s",
                         db_rules[i].sub);
            hits++;
        }
    }
    a->deep = 1;
}

/* ---------------------------------------------------------- hash tables -- */

static int hex_ok(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static void to_lower_hex(char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] >= 'A' && s[i] <= 'F')
            s[i] = (char)(s[i] - 'A' + 'a');
    }
}

static const hash_entry_t *find_hash(const hash_entry_t *tab, int count,
                                     const char *hex)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(tab[i].hex, hex) == 0)
            return &tab[i];
    }
    return NULL;
}

/* -------------------------------------------------------------- cache -- */

static int cache_lookup(const char *path, const struct stat *st,
                        scde_scan_result_t *out)
{
    int i;

    for (i = 0; i < SCDE_SCAN_RING; i++) {
        if (!cache[i].used)
            continue;
        if (strcmp(cache[i].path, path) == 0 && cache[i].size == st->st_size &&
            cache[i].mtime == (int64_t)st->st_mtime) {
            *out = cache[i].res;
            out->flags |= SCDE_SCANFLAG_CACHED;
            return 1;
        }
    }
    return 0;
}

static void cache_store(const char *path, const struct stat *st,
                        const scde_scan_result_t *res)
{
    cache_entry_t *e = &cache[cache_next];
    size_t plen = strlen(path);

    if (plen >= SCDE_PATH_MAX)
        return;
    e->used = 1;
    memcpy(e->path, path, plen + 1);
    e->size = st->st_size;
    e->mtime = (int64_t)st->st_mtime;
    e->res = *res;
    e->res.flags &= ~SCDE_SCANFLAG_CACHED;
    cache_next = (cache_next + 1) % SCDE_SCAN_RING;
}

static void cache_drop(const char *path)
{
    int i;

    for (i = 0; i < SCDE_SCAN_RING; i++) {
        if (cache[i].used && strcmp(cache[i].path, path) == 0)
            memset(&cache[i], 0, sizeof(cache[i]));
    }
}

/* ------------------------------------------------------------- database -- */

static int db_path(char *out, size_t out_len, const char *dir,
                   const char *name)
{
    int n;

    n = snprintf(out, out_len, "%.*s/%s", (int)(out_len - 16), dir, name);
    if (n <= 0 || (size_t)n >= out_len)
        return SCDE_ERR_ARG;
    if (scde_path_is_safe(out) != SCDE_OK)
        return SCDE_ERR_ARG;
    return SCDE_OK;
}

/*
 * Opens a database file with the required hygiene: regular file, not a
 * symlink, not executable, inside a safe path.  Returns SCDE_ERR_NOT_FOUND
 * when it simply does not exist (that is fine), SCDE_ERR otherwise.
 */
static FILE *db_open(const char *full)
{
    struct stat st;

    if (lstat(full, &st) != 0)
        return NULL;
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        scde_audit_log(SCDE_AUDIT_POLICY_DENIED, full, 60,
                       "database is not a regular file");
        return NULL;
    }
    if (st.st_mode & 0111) {
        scde_audit_log(SCDE_AUDIT_POLICY_DENIED, full, 70,
                       "database must not be executable");
        return NULL;
    }
    return fopen(full, "r");
}

static int header_ok(char *line, size_t len, FILE *f)
{
    size_t i;

    for (i = 0; i < 4 && fgets(line, (int)len, f); i++) {
        if (line[0] == '#') {
            if (strncmp(line, "# scde security", 15) == 0)
                return 1;
            continue;
        }
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        return 0;                   /* first real line must be the header */
    }
    return 0;
}

static int load_rules(const char *dir)
{
    char full[640];
    char line[320];
    FILE *f;

    n_db_rules = 0;
    if (db_path(full, sizeof(full), dir, "rules.conf") != SCDE_OK)
        return SCDE_OK;
    f = db_open(full);
    if (!f)
        return SCDE_OK;
    if (!header_ok(line, sizeof(line), f)) {
        fclose(f);
        scde_audit_log(SCDE_AUDIT_POLICY_DENIED, full, 50,
                       "rules.conf header missing");
        return SCDE_OK;
    }
    while (fgets(line, sizeof(line), f) && n_db_rules < MAX_RULES) {
        char *bar1;
        char *bar2;
        char *end;
        char *needle;
        const char *cat;
        long w;
        int reason;
        int group;
        size_t slen;
        size_t i;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        bar1 = strchr(line, '|');
        if (!bar1)
            continue;
        bar2 = strchr(bar1 + 1, '|');
        if (!bar2)
            continue;
        *bar1 = '\0';
        *bar2 = '\0';
        w = strtol(line, &end, 10);
        if (end == line || *end != '\0' || w < 1 || w > 100)
            continue;
        cat = bar1 + 1;
        if (strcmp(cat, "destructive") == 0)
            reason = SCDE_REASON_SCRIPT_DESTRUCTIVE;
        else if (strcmp(cat, "priv") == 0)
            reason = SCDE_REASON_SCRIPT_PRIV;
        else if (strcmp(cat, "cred") == 0)
            reason = SCDE_REASON_SCRIPT_CRED;
        else if (strcmp(cat, "persist") == 0)
            reason = SCDE_REASON_SCRIPT_PERSIST;
        else if (strcmp(cat, "remote") == 0)
            reason = SCDE_REASON_SCRIPT_REMOTE;
        else if (strcmp(cat, "obfusc") == 0)
            reason = SCDE_REASON_SCRIPT_OBFUSC;
        else if (strcmp(cat, "name") == 0 || strcmp(cat, "path") == 0 ||
                 strcmp(cat, "type") == 0 || strcmp(cat, "perm") == 0 ||
                 strcmp(cat, "elf") == 0)
            reason = SCDE_REASON_SIGNATURE;
        else
            continue;
        needle = bar2 + 1;              /* *bar2 = '\0' terminated cat */
        slen = strlen(needle);
        while (slen > 0 && (needle[slen - 1] == '\n' ||
                            needle[slen - 1] == '\r'))
            needle[--slen] = '\0';
        if (slen == 0 || slen >= SUB_LEN)
            continue;
        for (i = 0; i < slen; i++) {
            if ((unsigned char)needle[i] < 0x20)
                break;
        }
        if (i != slen)
            continue;
        if (!reason_group(reason, &group) ||
            reason == SCDE_REASON_SIGNATURE)
            group = G_COUNT;        /* signature rules are uncapped */
        db_rules[n_db_rules].weight = (int)w;
        db_rules[n_db_rules].reason = reason;
        db_rules[n_db_rules].group = group;
        memcpy(db_rules[n_db_rules].sub, needle, slen + 1);
        n_db_rules++;
    }
    fclose(f);
    return SCDE_OK;
}

static int load_hashes(const char *dir, const char *name, int trusted)
{
    char full[640];
    char line[320];
    FILE *f;
    hash_entry_t *tab = trusted ? trust_hashes : bad_hashes;
    int *count = trusted ? &n_trust_hashes : &n_bad_hashes;

    *count = 0;
    if (db_path(full, sizeof(full), dir, name) != SCDE_OK)
        return SCDE_OK;
    f = db_open(full);
    if (!f)
        return SCDE_OK;
    if (!header_ok(line, sizeof(line), f)) {
        fclose(f);
        scde_audit_log(SCDE_AUDIT_POLICY_DENIED, full, 50,
                       "hash database header missing");
        return SCDE_OK;
    }
    while (fgets(line, sizeof(line), f) && *count < MAX_HASHES) {
        char *sp;
        size_t i;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        sp = line;
        for (i = 0; i < 64 && sp[i] && sp[i] != ' ' && sp[i] != '\t' &&
                    sp[i] != '\n';
             i++)
            ;
        if (i != 64 || !hex_ok(line, 64))
            continue;
        memcpy(tab[*count].hex, line, 64);
        tab[*count].hex[64] = '\0';
        to_lower_hex(tab[*count].hex, 64);
        sp = line + 64;
        while (*sp == ' ' || *sp == '\t')
            sp++;
        {
            size_t n = 0;

            while (sp[n] && sp[n] != '\n' && sp[n] != '\r' && n < 39)
                n++;
            memcpy(tab[*count].label, sp, n);
            tab[*count].label[n] = '\0';
        }
        (*count)++;
    }
    fclose(f);
    return SCDE_OK;
}

static int load_dbs(const char *dir)
{
    if (!dir || !*dir)
        return SCDE_ERR_ARG;

    /* Reload security databases from scratch. */
    memset(db_rules, 0, sizeof(db_rules));
    memset(bad_hashes, 0, sizeof(bad_hashes));
    memset(trust_hashes, 0, sizeof(trust_hashes));
    n_db_rules = 0;
    n_bad_hashes = 0;
    n_trust_hashes = 0;

    load_rules(dir);
    load_hashes(dir, "hashes.db", 0);
    load_hashes(dir, "trusted.db", 1);
    return SCDE_OK;
}

/* ------------------------------------------------------------- scanning -- */

static int status_from_score(int score, int bad_hash)
{
    if (bad_hash && scde_policy_get(SCDE_POL_BLOCK_KNOWN_BAD))
        return SCDE_THREAT_BLOCKED;
    if (score >= 75)
        return SCDE_THREAT_MALICIOUS;
    if (score >= 50)
        return SCDE_THREAT_SUSPICIOUS;
    if (score >= 20)
        return SCDE_THREAT_UNKNOWN;
    return SCDE_THREAT_SAFE;
}

int scde_scan_file(const char *path, scde_scan_result_t *result)
{
    scde_scan_result_t r;
    struct stat st;
    struct stat target_st;
    const struct stat *ps;
    acc_t a;
    unsigned char head[512];
    char hex[65];
    char detail[96];
    int fd = -1;
    int exec_bit;
    int is_elf = 0;
    int is_script = 0;
    int bad_hash = 0;
    int trusted_hash = 0;
    int score;
    int best_reason = SCDE_REASON_NONE;
    int mode;
    int is_link = 0;
    off_t read_size = 0;
    char link_target[SCDE_PATH_MAX];

    if (!path || !result)
        return SCDE_ERR_ARG;
    memset(result, 0, sizeof(*result));
    memset(&a, 0, sizeof(a));
    memset(&r, 0, sizeof(r));
    detail[0] = '\0';
    r.status = SCDE_THREAT_UNKNOWN;
    r.trust = SCDE_TRUST_UNKNOWN;
    r.hash_status = SCDE_HASH_NOT_COMPUTED;

    scde__ensure();
    mode = scde_security_get_scan_mode();
    link_target[0] = '\0';
    if (mode == SCDE_SCAN_OFF) {
        r.flags |= SCDE_SCANFLAG_SKIPPED;
        r.hash_status = SCDE_HASH_SKIPPED;
        snprintf(r.detail, sizeof(r.detail), "scanner off");
        *result = r;
        return SCDE_ERR_STATE;
    }

    if (scde_path_is_safe(path) != SCDE_OK) {
        r.status = SCDE_THREAT_MALICIOUS;
        r.risk = 90;
        r.reason = SCDE_REASON_PATH;
        snprintf(r.detail, sizeof(r.detail), "unsafe path rejected");
        *result = r;
        scde__stats()->threats++;
        scde_audit_log(SCDE_AUDIT_THREAT_DETECTED, path, 90, "unsafe path");
        return SCDE_OK;
    }

    if (lstat(path, &st) != 0) {
        r.status = SCDE_THREAT_UNKNOWN;
        r.reason = SCDE_REASON_IO;
        snprintf(r.detail, sizeof(r.detail), "missing or unreadable");
        *result = r;
        return SCDE_ERR_NOT_FOUND;
    }

    /* cache: path + size + mtime.  Symlinks are never cached: the result
       depends on the (mutable, possibly swapped) target while the key
       would only cover the link itself - a stale SAFE verdict otherwise. */
    is_link = S_ISLNK(st.st_mode) ? 1 : 0;
    if (!is_link && cache_lookup(path, &st, &r)) {
        scde__stats()->cache_hits++;
        *result = r;
        return SCDE_OK;
    }

    ps = &st;
    if (S_ISLNK(st.st_mode)) {
        char target[SCDE_PATH_MAX];
        char resolved[SCDE_PATH_MAX + 64];
        ssize_t tn;
        const char *slash;
        size_t plen;

        r.flags |= SCDE_SCANFLAG_SYMLINK;
        tn = readlink(path, target, sizeof(target) - 1);
        if (tn <= 0) {
            acc_add(&a, G_SYMLINK, 40, SCDE_REASON_SYMLINK);
        } else {
            target[tn] = '\0';
            if (target[0] == '/') {
                snprintf(resolved, sizeof(resolved), "%.*s",
                         (int)(sizeof(resolved) - 1), target);
            } else {
                slash = strrchr(path, '/');
                plen = slash ? (size_t)(slash - path) : 0;
                snprintf(resolved, sizeof(resolved), "%.*s/%.*s",
                         (int)plen, slash ? path : "", 
                         (int)(sizeof(resolved) - plen - 3), target);
            }
            acc_add(&a, G_SYMLINK, 40, SCDE_REASON_SYMLINK);
            if (sensitive_target(resolved) || strstr(target, "..") != NULL)
                acc_add(&a, G_SYMLINK, 45, SCDE_REASON_SYMLINK);
            if (stat(path, &target_st) == 0 && S_ISREG(target_st.st_mode)) {
                ps = &target_st;
                /* read the content through the resolved target (single
                   level only: open() below uses O_NOFOLLOW so a chained
                   link fails closed instead of being followed) */
                snprintf(link_target, sizeof(link_target), "%.*s",
                         (int)(sizeof(link_target) - 1), resolved);
            } else {
                acc_add(&a, G_SYMLINK, 30, SCDE_REASON_SYMLINK);
            }
        }
    } else if (!S_ISREG(st.st_mode)) {
        r.status = SCDE_THREAT_UNKNOWN;
        r.reason = SCDE_REASON_TYPE;
        snprintf(r.detail, sizeof(r.detail), "not a regular file");
        scde__stats()->scans++;
        *result = r;
        if (scde_policy_get(SCDE_POL_AUDIT_FILE_SCANS))
            scde_audit_log(SCDE_AUDIT_FILE_SCANNED, path, 0, "not a file");
        return SCDE_OK;
    }

    exec_bit = (ps->st_mode & 0111) != 0;
    if (exec_bit)
        r.flags |= SCDE_SCANFLAG_EXEC;

    /* metadata: permissions */
    if (ps->st_mode & 04000)
        acc_add(&a, G_PERM, 35, SCDE_REASON_PERM);
    if (ps->st_mode & 02000)
        acc_add(&a, G_PERM, 25, SCDE_REASON_PERM);
    if ((ps->st_mode & 00002) && exec_bit)
        acc_add(&a, G_PERM, 25, SCDE_REASON_PERM);

    /* metadata: name */
    if (double_extension(path)) {
        acc_add(&a, G_NAME, 30, SCDE_REASON_NAME);
        snprintf(detail, sizeof(detail), "double extension");
    }

    /* metadata: location (executables in world-writable temp dirs) */
    if (path_group_for(path, exec_bit || ext_is_exec(path)))
        acc_add(&a, G_PATH, 20, SCDE_REASON_PATH);

    /* type inspection from the first bytes.  Symlinks are read through the
       resolved target captured above; the result is never cached, so a
       swapped target is always re-read on the next scan. */
    fd = open(is_link && link_target[0] ? link_target : path,
              O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        acc_add(&a, G_TYPE, 30, SCDE_REASON_IO);
        snprintf(detail, sizeof(detail), "cannot read");
    } else {
        struct stat fd_st;

        read_size = ps->st_size;
        /* size from the descriptor we actually read, not from the
           earlier stat: the target may have changed in between */
        if (fstat(fd, &fd_st) == 0 && S_ISREG(fd_st.st_mode)) {
            read_size = fd_st.st_size;
        } else {
            close(fd);
            fd = -1;
            acc_add(&a, G_TYPE, 30, SCDE_REASON_IO);
            snprintf(detail, sizeof(detail), "cannot read");
        }
        if (fd >= 0) {
        ssize_t hn = pread(fd, head, sizeof(head), 0);
        size_t n = hn > 0 ? (size_t)hn : 0;

        if (n >= 4 && head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' &&
            head[3] == 'F') {
            is_elf = 1;
            r.flags |= SCDE_SCANFLAG_ELF;
            elf_scan(fd, read_size, head, &a, path, exec_bit);
            a.deep = 1;
        } else if (n >= 2 && head[0] == '#' && head[1] == '!') {
            is_script = 1;
            r.flags |= SCDE_SCANFLAG_SCRIPT;
        } else if (n > 0 && looks_text(head, n)) {
            /* plain text: still worth the signature rules on explicit
               scans; the watcher never gets here (not relevant) */
            is_script = 0;
        } else if (n > 0) {
            /* binary that is not ELF */
            if (suffix_ci(path, ".so")) {
                acc_add(&a, G_TYPE, 55, SCDE_REASON_TYPE);
                snprintf(detail, sizeof(detail), "not an ELF shared object");
            } else if (exec_bit) {
                acc_add(&a, G_TYPE, 25, SCDE_REASON_TYPE);
                snprintf(detail, sizeof(detail), "unknown executable type");
            }
        }

        /* content rules: scripts, or any small text file.  A shebang file
           is always worth the rules - even when it contains NUL bytes
           (which would otherwise evade every signature) or non-ASCII
           text that fails the plain-text heuristic. */
        if (!is_elf && n > 0 && (looks_text(head, n) || is_script) &&
            read_size <= (off_t)DEEP_SIZE_MAX) {
            static unsigned char body[DEEP_SIZE_MAX + 1];
            size_t total = (size_t)read_size;
            ssize_t got;

            if (is_script)
                r.flags |= SCDE_SCANFLAG_SCRIPT;
            if (total > DEEP_SIZE_MAX)
                total = DEEP_SIZE_MAX;
            got = pread(fd, body, total, 0);
            if (got > 0) {
                body[got] = '\0';
                if (is_script && !looks_text(body, (size_t)got))
                    acc_add(&a, G_TYPE, 20, SCDE_REASON_TYPE);
                if (is_script || exec_bit || n_db_rules > 0 ||
                    r.flags & SCDE_SCANFLAG_SYMLINK)
                    script_scan(body, (size_t)got, &a, detail,
                                sizeof(detail));
            }
        }
        } /* end if (fd >= 0): content only from a verified descriptor */
    }

    /* hash + threat intelligence */
    if (read_size >= 0 && (uint64_t)read_size <= SCAN_SIZE_MAX && fd >= 0) {
        sha256_ctx ctx;
        unsigned char io[8192];
        ssize_t got;
        off_t off = 0;
        size_t total = (size_t)read_size;

        sha256_init(&ctx);
        while ((size_t)off < total) {
            size_t want = sizeof(io);

            if ((size_t)(total - off) < want)
                want = (size_t)(total - off);
            got = pread(fd, io, want, off);
            if (got <= 0)
                break;
            sha256_update(&ctx, io, (size_t)got);
            off += got;
        }
        sha256_final(&ctx, hex);
        r.hash_status = SCDE_HASH_COMPUTED;
        if (find_hash(bad_hashes, n_bad_hashes, hex)) {
            bad_hash = 1;
            r.hash_status = SCDE_HASH_KNOWN_BAD;
            a.definitive = 1;
            snprintf(detail, sizeof(detail), "known-bad hash");
        } else if (find_hash(trust_hashes, n_trust_hashes, hex)) {
            trusted_hash = 1;
            r.hash_status = SCDE_HASH_KNOWN_TRUSTED;
            r.trust = SCDE_TRUST_TRUSTED;
        }
    } else {
        r.hash_status = SCDE_HASH_SKIPPED;
    }
    if (fd >= 0)
        close(fd);

    /* fold the score */
    score = fold_risk(&a, &best_reason);
    if (a.definitive && bad_hash)
        score = 100;
    /* heuristic guard: one indicator group can never look like malware */
    if (!a.definitive && score >= 75 && popcount32(a.mask) < 2)
        score = 74;
    if (trusted_hash && score < 75 && score > 15)
        score = 15;
    if (score > 100)
        score = 100;

    if (bad_hash) {
        r.reason = SCDE_REASON_HASH;
        r.status = SCDE_THREAT_BLOCKED;
        if (!scde_policy_get(SCDE_POL_BLOCK_KNOWN_BAD)) {
            r.status = SCDE_THREAT_MALICIOUS;
            score = 100;
        }
    } else {
        if (a.sig_risk > 0 && best_reason != SCDE_REASON_SIGNATURE)
            best_reason = a.sig_risk >= score ? SCDE_REASON_SIGNATURE
                                              : best_reason;
        r.status = (int16_t)status_from_score(score, 0);
        r.reason = (int16_t)best_reason;
    }
    r.risk = (int16_t)score;
    if (a.deep)
        r.flags |= SCDE_SCANFLAG_DEEP;
    snprintf(r.detail, sizeof(r.detail), "%.*s",
             (int)(sizeof(r.detail) - 1),
             detail[0] ? detail : (bad_hash ? "known-bad hash" : "-"));

    /* trust from the path trust table (hash trust already reflected) */
    if (r.trust == SCDE_TRUST_UNKNOWN)
        r.trust = (int16_t)scde_trust_get(path);

    scde__stats()->scans++;
    if (r.status >= SCDE_THREAT_SUSPICIOUS) {
        scde__stats()->threats++;
        scde_audit_log(SCDE_AUDIT_THREAT_DETECTED, path, score, r.detail);
        if (r.status == SCDE_THREAT_BLOCKED)
            scde__stats()->blocked++;
    }
    if (scde_policy_get(SCDE_POL_AUDIT_FILE_SCANS))
        scde_audit_log(SCDE_AUDIT_FILE_SCANNED, path, score, r.detail);
    if (r.status == SCDE_THREAT_BLOCKED)
        scde_audit_log(SCDE_AUDIT_THREAT_BLOCKED, path, score, r.detail);

    if (!is_link)
        cache_store(path, &st, &r);
    *result = r;
    return SCDE_OK;
}

int scde_hash_file(const char *path, char out_hex[65])
{
    sha256_ctx ctx;
    unsigned char io[8192];
    struct stat st;
    ssize_t got;
    off_t off = 0;
    int fd;

    if (!path || !out_hex)
        return SCDE_ERR_ARG;
    out_hex[0] = '\0';
    if (scde_path_is_safe(path) != SCDE_OK)
        return SCDE_ERR_ARG;
    if (lstat(path, &st) != 0)
        return SCDE_ERR_NOT_FOUND;
    if (!S_ISREG(st.st_mode))
        return SCDE_ERR;
    if ((uint64_t)st.st_size > SCAN_SIZE_MAX)
        return SCDE_ERR_FULL;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return SCDE_ERR_IO;

    sha256_init(&ctx);
    while ((got = pread(fd, io, sizeof(io), off)) > 0) {
        sha256_update(&ctx, io, (size_t)got);
        off += got;
    }
    close(fd);
    sha256_final(&ctx, out_hex);
    return SCDE_OK;
}

/* ------------------------------------------------------------ API bits -- */

int scde_security_set_scan_mode(scde_scan_mode_t mode)
{
    scde_scan_mode_t old = (scde_scan_mode_t)scan_mode;

    if (mode != SCDE_SCAN_ALWAYS_ON && mode != SCDE_SCAN_PASSIVE &&
        mode != SCDE_SCAN_OFF)
        return SCDE_ERR_ARG;
    scan_mode = mode;
    if (mode != SCDE_SCAN_ALWAYS_ON)
        scde__scanner_watch_stop();
    if (old != mode) {
        scde_audit_log(SCDE_AUDIT_SCANNER_MODE_CHANGED, scde_scan_mode_name(mode),
                       0, "mode changed");
    }
    return SCDE_OK;
}

scde_scan_mode_t scde_security_get_scan_mode(void)
{
    return (scde_scan_mode_t)scan_mode;
}

const char *scde_scan_mode_name(scde_scan_mode_t mode)
{
    switch (mode) {
    case SCDE_SCAN_ALWAYS_ON:
        return "always-on";
    case SCDE_SCAN_PASSIVE:
        return "passive";
    case SCDE_SCAN_OFF:
        return "off";
    default:
        return "unknown";
    }
}

int scde_security_db_info(int *rules, int *hashes, int *trusted)
{
    if (rules)
        *rules = n_db_rules;
    if (hashes)
        *hashes = n_bad_hashes;
    if (trusted)
        *trusted = n_trust_hashes;
    return SCDE_OK;
}

int scde__scanner_init(const char *dir)
{
    if (!scanner_ready) {
        memset(cache, 0, sizeof(cache));
        cache_next = 0;
        scan_mode = SCDE_SCAN_PASSIVE;
        scanner_ready = 1;
    }
    return load_dbs(dir);
}

void scde__scanner_shutdown(void)
{
    scde__scanner_watch_stop();
    memset(cache, 0, sizeof(cache));
    memset(db_rules, 0, sizeof(db_rules));
    memset(bad_hashes, 0, sizeof(bad_hashes));
    memset(trust_hashes, 0, sizeof(trust_hashes));
    n_db_rules = 0;
    n_bad_hashes = 0;
    n_trust_hashes = 0;
    cache_next = 0;
}

int scde__scanner_reload(void)
{
    scde__ensure();
    return load_dbs(scde_security_db_dir());
}

/* ------------------------------------------------------------ watching -- */

int scde__scanner_watch_dir(const char *dir)
{
    int wd;
    size_t dlen;

    if (scan_mode != SCDE_SCAN_ALWAYS_ON)
        return SCDE_ERR_STATE;
    if (!dir || !*dir || scde_path_is_safe(dir) != SCDE_OK)
        return SCDE_ERR_ARG;
    dlen = strlen(dir);
    if (dlen >= sizeof(watch_dirs[0]))
        return SCDE_ERR_ARG;
    if (n_watch >= SCDE_WATCH_MAX)
        return SCDE_ERR_FULL;
    {
        int i;

        for (i = 0; i < n_watch; i++) {
            if (strcmp(watch_dirs[i], dir) == 0)
                return SCDE_OK;
        }
    }
    if (ifd < 0) {
        ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ifd < 0) {
            scde_log(SCDE_LOG_WARN, "security: inotify unavailable");
            return SCDE_ERR_IO;
        }
    }
    wd = inotify_add_watch(ifd, dir,
                           IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO |
                           IN_MOVED_FROM | IN_ATTRIB | IN_DELETE);
    if (wd < 0)
        return SCDE_ERR_IO;
    snprintf(watch_dirs[n_watch], sizeof(watch_dirs[0]), "%.*s",
             (int)(sizeof(watch_dirs[0]) - 1), dir);
    watch_wd[n_watch] = wd;
    n_watch++;
    scde_logf(SCDE_LOG_INFO, "security: watching %s", dir);
    return SCDE_OK;
}

void scde__scanner_watch_stop(void)
{
    int i;

    for (i = 0; i < n_watch; i++) {
        if (ifd >= 0)
            inotify_rm_watch(ifd, watch_wd[i]);
    }
    n_watch = 0;
    if (ifd >= 0) {
        close(ifd);
        ifd = -1;
    }
}

int scde__scanner_watch_fd(void)
{
    return ifd;
}

static const char *dir_for_wd(int wd)
{
    int i;

    for (i = 0; i < n_watch; i++) {
        if (watch_wd[i] == wd)
            return watch_dirs[i];
    }
    return NULL;
}

static int relevant_name(const char *name, const struct stat *st)
{
    if (st && (st->st_mode & 0111))
        return 1;
    return ext_is_exec(name);
}

static void handle_watch(const char *dir, const char *name, uint32_t mask)
{
    char path[SCDE_PATH_MAX];
    struct stat st;
    int n;

    if (!name[0])
        return;
    n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return;
    if (mask & (IN_DELETE | IN_MOVED_FROM)) {
        cache_drop(path);
        return;
    }
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if (!relevant_name(name, &st))
        return;                     /* never scan the world */

    {
        scde_scan_result_t r;

        scde_scan_file(path, &r);   /* quick -> cache -> rules -> score */
    }
}

int scde__scanner_pump(void)
{
    _Alignas(struct inotify_event) static char evbuf[4096];
    int processed = 0;

    if (ifd < 0)
        return SCDE_ERR_STATE;

    for (;;) {
        ssize_t n = read(ifd, evbuf, sizeof(evbuf));
        size_t off = 0;

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        while (off + sizeof(struct inotify_event) <= (size_t)n &&
               processed < 16) {
            const struct inotify_event *ev =
                (const struct inotify_event *)(const void *)(evbuf + off);
            const char *dir = dir_for_wd(ev->wd);

            if (dir && ev->len > 0)
                handle_watch(dir, ev->name, ev->mask);
            scde__stats()->watch_events++;
            processed++;
            off += sizeof(struct inotify_event) + ev->len;
        }
        break;                      /* bounded: one read per pump */
    }
    return processed;
}
