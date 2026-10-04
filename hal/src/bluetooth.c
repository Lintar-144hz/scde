#include <hal/bluetooth.h>

#include <string.h>

#include "backend.h"
#include "hal_internal.h"

static const hal_bt_backend_t *g_ops;
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

static int null_adapter_count(void)
{
    return 0;
}

static hal_status_t null_adapter_get(int index, hal_bt_adapter_t *out)
{
    (void)index;
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_fail_on(bool on)
{
    (void)on;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_fail_out(bool *out)
{
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static const hal_bt_backend_t null_backend = {
    "null",
    null_probe,
    null_adapter_count,
    null_adapter_get,
    null_fail_on,
    null_fail_out,
    null_fail_on
};

const hal_bt_backend_t *hal_bluetooth_null_backend(void)
{
    return &null_backend;
}

static hal_status_t ensure_ready(void)
{
    if (!hal_check_init()) {
        return HAL_ERR_INVALID;
    }
    if (!hal_module_enabled(HAL_MODULE_BLUETOOTH)) {
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

void hal_bluetooth_module_init(void)
{
    if (hal_force_unavailable()) {
        g_ops = hal_bluetooth_null_backend();
        g_available = false;
        g_caps = 0;
        return;
    }
    g_ops = hal_backend_bluetooth();
    if (g_ops == NULL) {
        g_ops = hal_bluetooth_null_backend();
    }
    g_available = false;
    g_caps = 0;
    (void)g_ops->probe(&g_available, &g_caps);
}

void hal_bluetooth_module_reset(void)
{
    g_ops = NULL;
    g_available = false;
    g_caps = 0;
}

const char *hal_bluetooth_backend_name(void)
{
    if (g_ops == NULL) {
        return "none";
    }
    return g_ops->name;
}

uint32_t hal_bluetooth_capabilities(void)
{
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_caps;
}

bool hal_bluetooth_available(void)
{
    return ensure_ready() == HAL_OK;
}

int hal_bluetooth_adapter_count(void)
{
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_ops->adapter_count();
}

hal_status_t hal_bluetooth_get_adapter(int index, hal_bt_adapter_t *out)
{
    hal_status_t st;

    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    if (index < 0 || index >= g_ops->adapter_count()) {
        return HAL_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    return g_ops->adapter_get(index, out);
}

hal_status_t hal_bluetooth_set_powered(bool on)
{
    hal_status_t st = ensure_ready();

    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_BT_CAP_POWER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->set_powered(on);
}

hal_status_t hal_bluetooth_get_powered(bool *on)
{
    hal_status_t st;

    if (on == NULL) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_BT_CAP_POWER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->get_powered(on);
}

hal_status_t hal_bluetooth_set_discovery(bool on)
{
    hal_status_t st = ensure_ready();

    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_BT_CAP_DISCOVERY) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->set_discovery(on);
}
