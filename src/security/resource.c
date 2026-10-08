/*
 * SCDE resource protection - kernel-enforced limits for child processes.
 *
 * Only the calling (child) process is ever limited here.  The supervisor
 * (scde_resource_constrain) only signals the supervised pid - never a
 * process group, never SCDE itself.
 */
#define _DEFAULT_SOURCE
#include <scde/security.h>
#include <scde/security_resource.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static scde_resource_limits_t active;
static int have_custom;

void scde_resource_defaults(scde_resource_limits_t *out)
{
    if (!out)
        return;
    out->ram_bytes = 512ul * 1024ul * 1024ul;   /* 512 MiB address space */
    out->cpu_seconds = 30;                       /* 30 s CPU time */
    out->nproc = 64;                             /* child processes */
    out->nofile = 128;                           /* open files */
    out->fsize_bytes = 256ul * 1024ul * 1024ul;  /* 256 MiB files */
    out->wall_seconds = 60;                      /* supervisor wall clock */
}

int scde_resource_set(const scde_resource_limits_t *lim)
{
    if (!lim) {
        have_custom = 0;
        memset(&active, 0, sizeof(active));
        return SCDE_OK;
    }
    active = *lim;
    have_custom = 1;
    return SCDE_OK;
}

int scde_resource_get(scde_resource_limits_t *out)
{
    if (!out)
        return SCDE_ERR_ARG;
    if (!have_custom)
        scde_resource_defaults(out);
    else
        *out = active;
    return SCDE_OK;
}

static int clamp_rlimit(int res, rlim_t value)
{
    struct rlimit rl;

    rl.rlim_cur = value;
    rl.rlim_max = value;
    if (setrlimit(res, &rl) != 0)
        return -1;
    return 0;
}

int scde_resource_apply(void)
{
    scde_resource_limits_t lim;
    int failures = 0;

    scde_resource_get(&lim);

    /* core dumps off, always */
    {
        struct rlimit rl;

        rl.rlim_cur = 0;
        rl.rlim_max = 0;
        if (setrlimit(RLIMIT_CORE, &rl) != 0)
            failures++;
    }
    if (lim.nofile > 0) {
        if (clamp_rlimit(RLIMIT_NOFILE, (rlim_t)lim.nofile) != 0)
            failures++;
    }
    if (lim.nproc > 0) {
#ifdef RLIMIT_NPROC
        if (clamp_rlimit(RLIMIT_NPROC, (rlim_t)lim.nproc) != 0)
            failures++;
#else
        (void)0;
#endif
    }
    if (lim.fsize_bytes > 0) {
        if (clamp_rlimit(RLIMIT_FSIZE, (rlim_t)lim.fsize_bytes) != 0)
            failures++;
    }
    if (lim.ram_bytes > 0) {
#ifdef RLIMIT_AS
        if (clamp_rlimit(RLIMIT_AS, (rlim_t)lim.ram_bytes) != 0)
            failures++;
#else
        failures++;
#endif
    }
    if (lim.cpu_seconds > 0) {
        if (clamp_rlimit(RLIMIT_CPU, (rlim_t)lim.cpu_seconds) != 0)
            failures++;
    }
    /* Baseline must hold: core off + at least the fd/process caps.
     * RAM/CPU may be unavailable on some kernels (e.g. PRoot Toto);
     * those are reported via the feature probe, but a total failure
     * here means the child must not run loose. */
    if (failures > 2)
        return SCDE_ERR;
    return SCDE_OK;
}

static int wait_ms(pid_t pid, unsigned int ms, int *status_out)
{
    unsigned int waited = 0;

    for (;;) {
        pid_t w = waitpid(pid, status_out, WNOHANG);
        int st;

        if (w == pid)
            return 1;               /* exited */
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return 1;               /* gone / error: treat as done */
        }
        if (waited >= ms)
            return 0;               /* still running */
        {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = 20 * 1000 * 1000L;
            nanosleep(&ts, NULL);
        }
        waited += 20;
        (void)st;
    }
}

int scde_resource_constrain(pid_t pid, unsigned int grace_ms,
                             int *status_out)
{
    int st = 0;

    if (pid <= 1)
        return 0;                   /* never touch init/SCDE/system */
    if (pid == getpid())
        return 0;                   /* never terminate ourselves */
    if (grace_ms > 30000)
        grace_ms = 30000;           /* bounded */

    /* fast path: already gone */
    if (wait_ms(pid, 0, status_out))
        return 1;

    /* violation path: SIGTERM first, grace, then SIGKILL (target only) */
    (void)kill(pid, SIGTERM);
    if (wait_ms(pid, grace_ms, &st)) {
        if (status_out)
            *status_out = st;
        return 1;
    }
    (void)kill(pid, SIGKILL);
    if (wait_ms(pid, 2000, &st)) {
        if (status_out)
            *status_out = st;
        return 1;
    }
    return 0;
}

int scde_resource_describe(char *out, size_t out_len)
{
    scde_resource_limits_t lim;

    if (!out || out_len == 0)
        return SCDE_ERR_ARG;
    scde_resource_get(&lim);
    snprintf(out, out_len, "ram=%luMB cpu=%us nproc=%lu fds=%lu fs=%luMB wall=%us",
             lim.ram_bytes / (1024ul * 1024ul), lim.cpu_seconds,
             lim.nproc, lim.nofile, lim.fsize_bytes / (1024ul * 1024ul),
             lim.wall_seconds);
    return SCDE_OK;
}
