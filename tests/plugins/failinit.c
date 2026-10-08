/*
 * Test plugin: valid metadata, but init() fails on purpose so the host can
 * prove that a failing plugin is isolated and never marked running.
 */
#include <scde/app.h>
#include <scde/plugin.h>

static const scde_plugin_info_t plugin_info = {
    .info_size = sizeof(scde_plugin_info_t),
    .abi_version = SCDE_PLUGIN_ABI_VERSION,
    .name = "failinit",
    .version = "0.1.0",
    .description = "Fails during init() on purpose",
    .api_major = SCDE_API_VERSION_MAJOR,
    .api_minor = SCDE_API_VERSION_MINOR
};

SCDE_PLUGIN_EXPORT const scde_plugin_info_t *scde_plugin_info(void)
{
    return &plugin_info;
}

SCDE_PLUGIN_EXPORT int scde_plugin_init(const scde_api_t *api)
{
    (void)api;
    return -7;
}

SCDE_PLUGIN_EXPORT void scde_plugin_shutdown(void)
{
    /* must never run: init() failed */
}
