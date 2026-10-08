/*
 * SCDE example plugin - the smallest useful plugin: reports its identity,
 * checks the API version, logs, posts one notification, shuts down.
 *
 * Built as a shared object and loaded through dlopen(); it uses the host
 * API only through the scde_api_t table, never through SCDE globals.
 */
#include <scde/app.h>
#include <scde/plugin.h>

static const scde_api_t *host_api;

static const scde_plugin_info_t plugin_info = {
    .info_size = sizeof(scde_plugin_info_t),
    .abi_version = SCDE_PLUGIN_ABI_VERSION,
    .name = "example",
    .version = "1.0.0",
    .description = "Minimal SCDE example plugin",
    .api_major = SCDE_API_VERSION_MAJOR,
    .api_minor = SCDE_API_VERSION_MINOR
};

SCDE_PLUGIN_EXPORT const scde_plugin_info_t *scde_plugin_info(void)
{
    return &plugin_info;
}

SCDE_PLUGIN_EXPORT int scde_plugin_init(const scde_api_t *api)
{
    if (!api)
        return -1;
    if (api->version_check(SCDE_API_VERSION_MAJOR, SCDE_API_VERSION_MINOR)
        != SCDE_OK)
        return -2;

    host_api = api;
    api->log(SCDE_LOG_INFO, "example: initialized");
    api->notify("Example plugin", "example plugin is running");
    return 0;
}

SCDE_PLUGIN_EXPORT void scde_plugin_shutdown(void)
{
    if (!host_api)
        return;
    host_api->log(SCDE_LOG_INFO, "example: shutdown");
    host_api = NULL;
}
