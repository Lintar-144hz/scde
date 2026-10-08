/*
 * Test plugin: reports an impossible required API version (99.0) so the
 * host must reject it during validation, before init() is ever called.
 */
#include <scde/app.h>
#include <scde/plugin.h>

static const scde_plugin_info_t plugin_info = {
    .info_size = sizeof(scde_plugin_info_t),
    .abi_version = SCDE_PLUGIN_ABI_VERSION,
    .name = "badver",
    .version = "9.9.9",
    .description = "Requires a future API version",
    .api_major = 99,
    .api_minor = 0
};

SCDE_PLUGIN_EXPORT const scde_plugin_info_t *scde_plugin_info(void)
{
    return &plugin_info;
}

SCDE_PLUGIN_EXPORT int scde_plugin_init(const scde_api_t *api)
{
    (void)api;
    return 0;                      /* never reached: version check fails */
}

SCDE_PLUGIN_EXPORT void scde_plugin_shutdown(void)
{
}
