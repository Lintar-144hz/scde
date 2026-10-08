/*
 * SCDE plugin loader - shared library loading and ABI validation.
 */
#include "loader.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err && err_len > 0)
        snprintf(err, err_len, "%s", msg ? msg : "unknown error");
}

/* smallest info struct this host understands (v1 layout) */
static size_t info_min_size(void)
{
    return offsetof(scde_plugin_info_t, api_minor) + sizeof(uint32_t);
}

int scde_loader_open(const char *path, scde_loaded_plugin_t *out, char *err,
                     size_t err_len)
{
    union { void *p; scde_plugin_info_fn f; } u_info;
    union { void *p; scde_plugin_init_fn f; } u_init;
    union { void *p; scde_plugin_shutdown_fn f; } u_shutdown;
    const scde_plugin_info_t *info;
    void *dl;
    char msg[192];

    if (!path || !out)
        return SCDE_ERR_ARG;
    memset(out, 0, sizeof(*out));
    if (err && err_len > 0)
        err[0] = '\0';

    dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        const char *de = dlerror();

        snprintf(msg, sizeof(msg), "dlopen failed: %s",
                 de ? de : "unknown error");
        set_err(err, err_len, msg);
        return SCDE_ERR_IO;
    }

    u_info.p = dlsym(dl, SCDE_PLUGIN_ENTRY_INFO);
    u_init.p = dlsym(dl, SCDE_PLUGIN_ENTRY_INIT);
    u_shutdown.p = dlsym(dl, SCDE_PLUGIN_ENTRY_SHUTDOWN);
    if (!u_info.p || !u_init.p || !u_shutdown.p) {
        snprintf(msg, sizeof(msg), "missing entry point(s): %s%s%s",
                 u_info.p ? "" : SCDE_PLUGIN_ENTRY_INFO " ",
                 u_init.p ? "" : SCDE_PLUGIN_ENTRY_INIT " ",
                 u_shutdown.p ? "" : SCDE_PLUGIN_ENTRY_SHUTDOWN);
        set_err(err, err_len, msg);
        dlclose(dl);
        return SCDE_ERR_NOT_FOUND;
    }

    info = u_info.f();
    if (!info) {
        set_err(err, err_len, "scde_plugin_info() returned NULL");
        dlclose(dl);
        return SCDE_ERR_VERSION;
    }

    /* layout first - never read fields we cannot trust yet */
    if (info->info_size < info_min_size()) {
        snprintf(msg, sizeof(msg), "info_size %u too small (need >= %zu)",
                 (unsigned)info->info_size, info_min_size());
        set_err(err, err_len, msg);
        dlclose(dl);
        return SCDE_ERR_VERSION;
    }

    /* v1-compatible from here on: keep the metadata for diagnostics even if
       validation below rejects the plugin (strings live in `out`, so they
       survive dlclose) */
    snprintf(out->name, sizeof(out->name), "%s",
             info->name ? info->name : "");
    snprintf(out->version, sizeof(out->version), "%s",
             info->version ? info->version : "");
    snprintf(out->description, sizeof(out->description), "%s",
             info->description ? info->description : "");
    out->abi_version = info->abi_version;
    out->api_major = info->api_major;
    out->api_minor = info->api_minor;
    out->view.info_size = sizeof(out->view);
    out->view.abi_version = out->abi_version;
    out->view.name = out->name;
    out->view.version = out->version;
    out->view.description = out->description;
    out->view.api_major = out->api_major;
    out->view.api_minor = out->api_minor;

    if (info->abi_version != SCDE_PLUGIN_ABI_VERSION) {
        snprintf(msg, sizeof(msg), "plugin ABI %u, host ABI %u",
                 (unsigned)info->abi_version,
                 (unsigned)SCDE_PLUGIN_ABI_VERSION);
        set_err(err, err_len, msg);
        dlclose(dl);
        return SCDE_ERR_VERSION;
    }
    if (!out->name[0]) {
        set_err(err, err_len, "plugin has no name");
        dlclose(dl);
        return SCDE_ERR_ARG;
    }
    if (!out->version[0]) {
        set_err(err, err_len, "plugin has no version");
        dlclose(dl);
        return SCDE_ERR_ARG;
    }
    if (scde_api_version_check(out->api_major, out->api_minor) != SCDE_OK) {
        snprintf(msg, sizeof(msg),
                 "requires API %u.%u, host provides %u.%u",
                 (unsigned)out->api_major, (unsigned)out->api_minor,
                 (unsigned)SCDE_API_VERSION_MAJOR,
                 (unsigned)SCDE_API_VERSION_MINOR);
        set_err(err, err_len, msg);
        dlclose(dl);
        return SCDE_ERR_VERSION;
    }

    out->init = u_init.f;
    out->shutdown = u_shutdown.f;
    out->dl = dl;
    return SCDE_OK;
}

int scde_loader_start(scde_loaded_plugin_t *p, char *err, size_t err_len)
{
    char msg[160];
    int rc;

    if (!p || !p->dl || !p->init)
        return SCDE_ERR_ARG;
    if (err && err_len > 0)
        err[0] = '\0';

    rc = p->init(scde_api_table());
    if (rc != 0) {
        snprintf(msg, sizeof(msg), "init failed (code %d)", rc);
        set_err(err, err_len, msg);
        return SCDE_ERR;
    }
    return SCDE_OK;
}

void scde_loader_close(scde_loaded_plugin_t *p)
{
    if (!p)
        return;
    if (p->dl)
        dlclose(p->dl);
    /* metadata (name/version/...) is kept: it is host-owned memory and
       stays useful for diagnostics after a close */
    p->dl = NULL;
    p->init = NULL;
    p->shutdown = NULL;
}

void scde_loader_stop(scde_loaded_plugin_t *p)
{
    if (!p)
        return;
    if (p->dl && p->shutdown)
        p->shutdown();
    scde_loader_close(p);
}
