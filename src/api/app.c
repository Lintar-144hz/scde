/*
 * SCDE App API - application lifecycle, metadata, services, plugin table.
 */
#include <scde/app.h>
#include <scde/config.h>
#include <scde/event.h>
#include <scde/notification.h>
#include <scde/security.h>
#include <scde/window.h>

#include "internal.h"

#include <stdio.h>
#include <string.h>

#define SVC_MAX 16
#define SVC_NAME_LEN 32

typedef struct {
    int used;
    char name[SVC_NAME_LEN];
    scde_service_fn fn;
    void *user;
} service_t;

static int app_running;
static scde_app_info_t app_info;
static char info_name[64];
static char info_version[32];
static char info_desc[128];
static char info_author[64];
static service_t services[SVC_MAX];

/* --------------------------------------------------- built-in services -- */

static int svc_version(const char *args, char *out, size_t out_len,
                       void *user)
{
    (void)args;
    (void)user;
    if (!out || out_len == 0)
        return SCDE_ERR_ARG;
    snprintf(out, out_len, "%s", SCDE_API_VERSION_STRING);
    return SCDE_OK;
}

static int svc_notify(const char *args, char *out, size_t out_len, void *user)
{
    char title[SCDE_NOTIFICATION_TITLE];
    const char *nl;

    (void)out;
    (void)out_len;
    (void)user;
    if (!args || !*args)
        return SCDE_ERR_ARG;
    nl = strchr(args, '\n');
    if (nl) {
        size_t n = (size_t)(nl - args);

        if (n >= sizeof(title))
            n = sizeof(title) - 1;
        memcpy(title, args, n);
        title[n] = '\0';
        return scde_notify(title, nl + 1);
    }
    return scde_notify(args, "");
}

static int svc_config_get(const char *args, char *out, size_t out_len,
                          void *user)
{
    const char *v;

    (void)user;
    if (!out || out_len == 0)
        return SCDE_ERR_ARG;
    out[0] = '\0';
    if (!args || !*args)
        return SCDE_ERR_ARG;
    v = scde_config_get(args, NULL);
    if (!v)
        return SCDE_ERR_NOT_FOUND;
    snprintf(out, out_len, "%s", v);
    return SCDE_OK;
}

static const struct {
    const char *name;
    scde_service_fn fn;
} builtins[] = {
    { "version", svc_version },
    { "notify", svc_notify },
    { "config.get", svc_config_get }
};

/* ----------------------------------------------------------- lifecycle -- */

static void clear_services(void)
{
    memset(services, 0, sizeof(services));
}

static void register_builtins(void)
{
    size_t i;

    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        scde_service_register(builtins[i].name, builtins[i].fn, NULL);
}

int scde_app_init(const scde_app_info_t *info)
{
    if (app_running)
        return SCDE_ERR_STATE;
    if (info && (!info->name || !*info->name))
        return SCDE_ERR_ARG;

    info_name[0] = '\0';
    info_version[0] = '\0';
    info_desc[0] = '\0';
    info_author[0] = '\0';
    if (info) {
        snprintf(info_name, sizeof(info_name), "%s", info->name);
        snprintf(info_version, sizeof(info_version), "%s",
                 info->version ? info->version : "");
        snprintf(info_desc, sizeof(info_desc), "%s",
                 info->description ? info->description : "");
        snprintf(info_author, sizeof(info_author), "%s",
                 info->author ? info->author : "");
    }
    app_info.name = info_name;
    app_info.version = info_version;
    app_info.description = info_desc;
    app_info.author = info_author;

    clear_services();
    register_builtins();
    app_running = 1;

    scde_logf(SCDE_LOG_INFO, "app: %s %s initialized (api %s)",
              info_name[0] ? info_name : "app",
              info_version[0] ? info_version : "",
              SCDE_API_VERSION_STRING);

    {
        scde_event_t ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = SCDE_EVENT_APP_INIT;
        snprintf(ev.text, sizeof(ev.text), "%s", info_name);
        scde_event_post(&ev);
    }
    return SCDE_OK;
}

void scde_app_shutdown(void)
{
    scde_event_t ev;

    if (!app_running)
        return;

    memset(&ev, 0, sizeof(ev));
    ev.type = SCDE_EVENT_APP_SHUTDOWN;
    snprintf(ev.text, sizeof(ev.text), "%s", info_name);
    scde_event_post(&ev);

    clear_services();
    app_running = 0;
    scde_api_close_display();
    scde_logf(SCDE_LOG_INFO, "app: %s shut down",
              info_name[0] ? info_name : "app");
    info_name[0] = '\0';
    info_version[0] = '\0';
    info_desc[0] = '\0';
    info_author[0] = '\0';
}

int scde_app_is_running(void)
{
    return app_running;
}

const scde_app_info_t *scde_app_info(void)
{
    return app_running ? &app_info : NULL;
}

/* ----------------------------------------------------------- services -- */

int scde_service_register(const char *name, scde_service_fn fn, void *user)
{
    int free_slot = -1;
    int i;

    if (!name || !*name || strlen(name) >= SVC_NAME_LEN || !fn)
        return SCDE_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (!services[i].used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (strcmp(services[i].name, name) == 0)
            return SCDE_ERR_STATE;
    }
    if (free_slot < 0)
        return SCDE_ERR_FULL;
    services[free_slot].used = 1;
    snprintf(services[free_slot].name, sizeof(services[free_slot].name), "%s",
             name);
    services[free_slot].fn = fn;
    services[free_slot].user = user;
    return SCDE_OK;
}

int scde_service_unregister(const char *name)
{
    int i;

    if (!name)
        return SCDE_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0) {
            memset(&services[i], 0, sizeof(services[i]));
            return SCDE_OK;
        }
    }
    return SCDE_ERR_NOT_FOUND;
}

int scde_service_call(const char *name, const char *args, char *out,
                      size_t out_len)
{
    int i;

    if (!name)
        return SCDE_ERR_ARG;
    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used && strcmp(services[i].name, name) == 0)
            return services[i].fn(args, out, out_len, services[i].user);
    }
    return SCDE_ERR_NOT_FOUND;
}

/*
 * Capability-mediated dispatch.  The trusted core path
 * (scde_service_call) stays untouched for ABI/behaviour stability;
 * untrusted callers go through here so forbidden capabilities are
 * denied and audited instead of silently allowed.
 */
int scde_service_call_as(const char *subject, const char *name,
                         const char *args, char *out, size_t out_len)
{
    uint32_t need = 0;

    if (!subject || !*subject || !name || !*name)
        return SCDE_ERR_ARG;
    if (strcmp(name, "version") == 0) {
        need = 0;                   /* public */
    } else if (strcmp(name, "notify") == 0) {
        need = SCDE_CAP_AUDIT;
    } else if (strcmp(name, "config.get") == 0) {
        need = SCDE_CAP_CONFIG;
    } else {
        return SCDE_ERR_NOT_FOUND;
    }
    if (need != 0 && scde_security_check(subject, need) != SCDE_OK)
        return SCDE_ERR;            /* audited as CAPABILITY_DENIED */
    return scde_service_call(name, args, out, out_len);
}

int scde_service_count(void)
{
    int i, n = 0;

    for (i = 0; i < SVC_MAX; i++) {
        if (services[i].used)
            n++;
    }
    return n;
}

/* ------------------------------------------------------- plugin table -- */

static const scde_api_t api_table = {
    .version_major = SCDE_API_VERSION_MAJOR,
    .version_minor = SCDE_API_VERSION_MINOR,
    .version_check = scde_api_version_check,
    .log = scde_log,
    .notify = scde_notify,
    .config_get = scde_config_get,
    .config_get_int = scde_config_get_int,
    .event_subscribe = scde_event_subscribe,
    .event_post = scde_event_post,
    .service_call = scde_service_call
};

const scde_api_t *scde_api_table(void)
{
    return &api_table;
}
