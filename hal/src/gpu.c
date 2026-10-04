#include <hal/gpu.h>

#include <string.h>

#include "backend.h"
#include "hal_internal.h"

static const hal_gpu_backend_t *g_ops;
static bool g_available;
static uint32_t g_caps;

static hal_status_t null_probe(bool *available, uint32_t *capabilities)
{
    if (available != NULL) {
        *available = false;
    }
    if (capabilities != NULL) {
        *capabilities = 0;
    }
    return HAL_ERR_UNAVAILABLE;
}

static int null_count(void)
{
    return 0;
}

static hal_status_t null_get(int index, hal_gpu_info_t *out)
{
    (void)index;
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static const hal_gpu_backend_t null_backend = {
    "null", null_probe, null_count, null_get
};

const hal_gpu_backend_t *hal_gpu_null_backend(void)
{
    return &null_backend;
}

static hal_status_t ensure_ready(void)
{
    if (!hal_check_init()) {
        return HAL_ERR_INVALID;
    }
    if (!hal_module_enabled(HAL_MODULE_GPU)) {
        return HAL_ERR_INVALID;
    }
    if (g_ops == NULL) {
        return HAL_ERR_INVALID;
    }
    if (!g_available) {
        return HAL_ERR_UNAVAILABLE;
    }
    return HAL_OK;
}

void hal_gpu_module_init(void)
{
    if (hal_force_unavailable()) {
        g_ops = hal_gpu_null_backend();
        g_available = false;
        g_caps = 0;
        return;
    }
    g_ops = hal_backend_gpu();
    if (g_ops == NULL) {
        g_ops = hal_gpu_null_backend();
    }
    g_available = false;
    g_caps = 0;
    (void)g_ops->probe(&g_available, &g_caps);
}

void hal_gpu_module_reset(void)
{
    g_ops = NULL;
    g_available = false;
    g_caps = 0;
}

const char *hal_gpu_backend_name(void)
{
    if (g_ops == NULL) {
        return "none";
    }
    return g_ops->name;
}

uint32_t hal_gpu_capabilities(void)
{
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_caps;
}

bool hal_gpu_available(void)
{
    return ensure_ready() == HAL_OK;
}

int hal_gpu_count(void)
{
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_ops->count();
}

hal_status_t hal_gpu_get_info(int index, hal_gpu_info_t *out)
{
    hal_status_t st;

    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    if (index < 0 || index >= g_ops->count()) {
        return HAL_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    return g_ops->get(index, out);
}

hal_status_t hal_gpu_primary(hal_gpu_info_t *out)
{
    return hal_gpu_get_info(0, out);
}
