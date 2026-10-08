/*
 * SCDE plugin loader - dlopen/dlsym/dlclose wrapper with validation.
 * Private to src/plugin; the public surface is include/scde/plugin.h.
 */
#ifndef SCDE_PLUGIN_LOADER_H
#define SCDE_PLUGIN_LOADER_H

#include <scde/plugin.h>

typedef struct scde_loaded_plugin {
    void *dl;                          /* dlopen handle, NULL when closed */
    char name[64];
    char version[32];
    char description[128];
    uint32_t abi_version;
    uint32_t api_major;
    uint32_t api_minor;
    scde_plugin_init_fn init;
    scde_plugin_shutdown_fn shutdown;
    /* metadata view; name/version/description point at the arrays above */
    scde_plugin_info_t view;
} scde_loaded_plugin_t;

/*
 * Opens path, resolves the three entries and validates the metadata
 * (info size, ABI, required API version).  On failure nothing is kept
 * open and err (if given) describes the problem; name/version may still
 * be filled in when the plugin got far enough to report them.
 */
int scde_loader_open(const char *path, scde_loaded_plugin_t *out, char *err,
                     size_t err_len);

/* init(): 0 = SCDE_OK, anything else is reported and left un-started. */
int scde_loader_start(scde_loaded_plugin_t *p, char *err, size_t err_len);

/* shutdown() (if started) then dlclose(); metadata is kept for diagnostics. */
void scde_loader_stop(scde_loaded_plugin_t *p);

/* dlclose() only - for plugins that never started successfully; also keeps
   the metadata. */
void scde_loader_close(scde_loaded_plugin_t *p);

#endif /* SCDE_PLUGIN_LOADER_H */
