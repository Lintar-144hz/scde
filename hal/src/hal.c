#include <hal/hal.h>

#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "hal_internal.h"

static struct {
    bool inited;
    uint32_t modules;
    uint32_t flags;
} g_state;

const char *hal_version(void)
{
    return "0.1";
}

const char *hal_status_str(hal_status_t status)
{
    switch (status) {
    case HAL_OK:
        return "ok";
    case HAL_ERR_UNAVAILABLE:
        return "unavailable";
    case HAL_ERR_UNSUPPORTED:
        return "unsupported";
    case HAL_ERR_PERMISSION:
        return "permission denied";
    case HAL_ERR_IO:
        return "i/o error";
    case HAL_ERR_INVALID:
        return "invalid argument";
    case HAL_ERR_NOMEM:
        return "out of memory";
    case HAL_ERR_BUSY:
        return "busy";
    }
    return "unknown";
}

bool hal_check_init(void)
{
    return g_state.inited;
}

bool hal_force_unavailable(void)
{
    return g_state.inited && (g_state.flags & HAL_INIT_FORCE_UNAVAILABLE) != 0;
}

size_t hal_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);

    if (cap != 0) {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static bool module_selected(hal_module_t module)
{
    return g_state.inited && (g_state.modules & (uint32_t)module) != 0;
}

bool hal_module_enabled(hal_module_t module)
{
    return module_selected(module);
}

hal_status_t hal_init(const hal_init_opts_t *opts)
{
    if (g_state.inited) {
        return HAL_ERR_BUSY;
    }

    g_state.modules = opts ? opts->modules : HAL_MODULE_ALL;
    g_state.modules &= HAL_MODULE_ALL;
    if (g_state.modules == 0) {
        g_state.modules = HAL_MODULE_ALL;
    }
    g_state.flags = opts ? opts->flags : 0;
    g_state.inited = true;

    if (module_selected(HAL_MODULE_GPU)) {
        hal_gpu_module_init();
    }
    if (module_selected(HAL_MODULE_BLUETOOTH)) {
        hal_bluetooth_module_init();
    }
    if (module_selected(HAL_MODULE_AUDIO)) {
        hal_audio_module_init();
    }
    return HAL_OK;
}

void hal_shutdown(void)
{
    if (!g_state.inited) {
        return;
    }
    hal_gpu_module_reset();
    hal_bluetooth_module_reset();
    hal_audio_module_reset();
    g_state.inited = false;
    g_state.modules = 0;
    g_state.flags = 0;
}

bool hal_is_inited(void)
{
    return g_state.inited;
}

bool hal_module_available(hal_module_t module)
{
    if (!module_selected(module)) {
        return false;
    }
    switch (module) {
    case HAL_MODULE_GPU:
        return hal_gpu_available();
    case HAL_MODULE_BLUETOOTH:
        return hal_bluetooth_available();
    case HAL_MODULE_AUDIO:
        return hal_audio_available();
    }
    return false;
}

uint32_t hal_available_mask(void)
{
    uint32_t mask = 0;

    if (hal_module_available(HAL_MODULE_GPU)) {
        mask |= HAL_MODULE_GPU;
    }
    if (hal_module_available(HAL_MODULE_BLUETOOTH)) {
        mask |= HAL_MODULE_BLUETOOTH;
    }
    if (hal_module_available(HAL_MODULE_AUDIO)) {
        mask |= HAL_MODULE_AUDIO;
    }
    return mask;
}

hal_status_t hal_module_get_info(hal_module_t module, hal_module_info_t *out)
{
    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    if (!g_state.inited) {
        return HAL_ERR_INVALID;
    }
    switch (module) {
    case HAL_MODULE_GPU:
    case HAL_MODULE_BLUETOOTH:
    case HAL_MODULE_AUDIO:
        break;
    default:
        return HAL_ERR_INVALID;
    }
    if (!module_selected(module)) {
        return HAL_ERR_INVALID;
    }

    memset(out, 0, sizeof(*out));
    out->module = module;
    if (module == HAL_MODULE_GPU) {
        out->name = "gpu";
    } else if (module == HAL_MODULE_BLUETOOTH) {
        out->name = "bluetooth";
    } else {
        out->name = "audio";
    }
    out->available = hal_module_available(module);
    switch (module) {
    case HAL_MODULE_GPU:
        out->backend = hal_gpu_backend_name();
        out->capabilities = hal_gpu_capabilities();
        break;
    case HAL_MODULE_BLUETOOTH:
        out->backend = hal_bluetooth_backend_name();
        out->capabilities = hal_bluetooth_capabilities();
        break;
    default:
        out->backend = hal_audio_backend_name();
        out->capabilities = hal_audio_capabilities();
        break;
    }
    return HAL_OK;
}
