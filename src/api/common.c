/*
 * SCDE App API - version, status codes, logging.
 */
#include <scde/common.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ version -- */

int scde_api_version_check(uint32_t want_major, uint32_t want_minor)
{
    if (want_major != SCDE_API_VERSION_MAJOR)
        return SCDE_ERR_VERSION;
    if (want_minor > SCDE_API_VERSION_MINOR)
        return SCDE_ERR_VERSION;
    return SCDE_OK;
}

const char *scde_api_version_string(void)
{
    return SCDE_API_VERSION_STRING;
}

/* -------------------------------------------------------- status codes -- */

const char *scde_strerror(int status)
{
    switch (status) {
    case SCDE_OK:
        return "success";
    case SCDE_ERR:
        return "generic failure";
    case SCDE_ERR_ARG:
        return "invalid argument";
    case SCDE_ERR_STATE:
        return "invalid state";
    case SCDE_ERR_DISPLAY:
        return "no display available";
    case SCDE_ERR_NOT_FOUND:
        return "not found";
    case SCDE_ERR_VERSION:
        return "version mismatch";
    case SCDE_ERR_IO:
        return "input/output error";
    case SCDE_ERR_FULL:
        return "table full";
    default:
        return "unknown status";
    }
}

/* ------------------------------------------------------------- logging -- */

static scde_log_sink_t log_sink;
static void *log_sink_user;

void scde_log_set_sink(scde_log_sink_t sink, void *user)
{
    log_sink = sink;
    log_sink_user = user;
}

const char *scde_log_level_name(scde_log_level_t level)
{
    switch (level) {
    case SCDE_LOG_DEBUG:
        return "debug";
    case SCDE_LOG_INFO:
        return "info";
    case SCDE_LOG_WARN:
        return "warn";
    case SCDE_LOG_ERROR:
        return "error";
    default:
        return "log";
    }
}

void scde_log(scde_log_level_t level, const char *message)
{
    if (log_sink) {
        log_sink(level, message ? message : "", log_sink_user);
        return;
    }
    fprintf(stderr, "[scde-api] %s: %s\n", scde_log_level_name(level),
            message ? message : "");
    fflush(stderr);
}

void scde_logf(scde_log_level_t level, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!fmt) {
        scde_log(level, "");
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    scde_log(level, buf);
}
