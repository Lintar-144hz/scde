#include "backend.h"

static const hal_bt_backend_t *lin(void)
{
    return hal_backend_bluetooth_linux();
}

static hal_status_t android_bt_probe(bool *available, uint32_t *capabilities)
{
    return lin()->probe(available, capabilities);
}

static int android_bt_adapter_count(void)
{
    return lin()->adapter_count();
}

static hal_status_t android_bt_adapter_get(int index, hal_bt_adapter_t *out)
{
    return lin()->adapter_get(index, out);
}

static hal_status_t android_bt_set_powered(bool on)
{
    return lin()->set_powered(on);
}

static hal_status_t android_bt_get_powered(bool *on)
{
    return lin()->get_powered(on);
}

static hal_status_t android_bt_set_discovery(bool on)
{
    return lin()->set_discovery(on);
}

static const hal_bt_backend_t android_backend = {
    "android",
    android_bt_probe,
    android_bt_adapter_count,
    android_bt_adapter_get,
    android_bt_set_powered,
    android_bt_get_powered,
    android_bt_set_discovery
};

const hal_bt_backend_t *hal_backend_bluetooth_android(void)
{
    return &android_backend;
}
