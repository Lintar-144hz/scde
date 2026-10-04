#include <hal/audio.h>

#include <string.h>

#include "backend.h"
#include "hal_internal.h"

static const hal_audio_backend_t *g_ops;
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

static int null_count(hal_audio_dir_t direction)
{
    (void)direction;
    return 0;
}

static hal_status_t null_get(int index, hal_audio_dir_t direction,
                             hal_audio_device_t *out)
{
    (void)index;
    (void)direction;
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_volume_get(float *out)
{
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_volume_set(float in)
{
    (void)in;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_muted_get(bool *out)
{
    (void)out;
    return HAL_ERR_UNAVAILABLE;
}

static hal_status_t null_muted_set(bool in)
{
    (void)in;
    return HAL_ERR_UNAVAILABLE;
}

static const hal_audio_backend_t null_backend = {
    "null",
    null_probe,
    null_count,
    null_get,
    null_volume_get,
    null_volume_set,
    null_muted_get,
    null_muted_set
};

const hal_audio_backend_t *hal_audio_null_backend(void)
{
    return &null_backend;
}

static hal_status_t ensure_ready(void)
{
    if (!hal_check_init()) {
        return HAL_ERR_INVALID;
    }
    if (!hal_module_enabled(HAL_MODULE_AUDIO)) {
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

void hal_audio_module_init(void)
{
    if (hal_force_unavailable()) {
        g_ops = hal_audio_null_backend();
        g_available = false;
        g_caps = 0;
        return;
    }
    g_ops = hal_backend_audio();
    if (g_ops == NULL) {
        g_ops = hal_audio_null_backend();
    }
    g_available = false;
    g_caps = 0;
    (void)g_ops->probe(&g_available, &g_caps);
}

void hal_audio_module_reset(void)
{
    g_ops = NULL;
    g_available = false;
    g_caps = 0;
}

const char *hal_audio_backend_name(void)
{
    if (g_ops == NULL) {
        return "none";
    }
    return g_ops->name;
}

uint32_t hal_audio_capabilities(void)
{
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_caps;
}

bool hal_audio_available(void)
{
    return ensure_ready() == HAL_OK;
}

int hal_audio_device_count(hal_audio_dir_t direction)
{
    if (direction != HAL_AUDIO_PLAYBACK && direction != HAL_AUDIO_CAPTURE) {
        return 0;
    }
    if (ensure_ready() != HAL_OK) {
        return 0;
    }
    return g_ops->count(direction);
}

hal_status_t hal_audio_get_device(int index, hal_audio_dir_t direction,
                                  hal_audio_device_t *out)
{
    hal_status_t st;
    int count;

    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    if (direction != HAL_AUDIO_PLAYBACK && direction != HAL_AUDIO_CAPTURE) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    count = g_ops->count(direction);
    if (index < 0 || index >= count) {
        return HAL_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    return g_ops->get(index, direction, out);
}

hal_status_t hal_audio_get_master_volume(float *out01)
{
    hal_status_t st;

    if (out01 == NULL) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_AUDIO_CAP_MIXER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->get_volume(out01);
}

hal_status_t hal_audio_set_master_volume(float in01)
{
    hal_status_t st = ensure_ready();

    if (st != HAL_OK) {
        return st;
    }
    if (in01 < 0.0f || in01 > 1.0f) {
        return HAL_ERR_INVALID;
    }
    if ((g_caps & HAL_AUDIO_CAP_MIXER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->set_volume(in01);
}

hal_status_t hal_audio_get_muted(bool *out)
{
    hal_status_t st;

    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    st = ensure_ready();
    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_AUDIO_CAP_MIXER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->get_muted(out);
}

hal_status_t hal_audio_set_muted(bool in)
{
    hal_status_t st = ensure_ready();

    if (st != HAL_OK) {
        return st;
    }
    if ((g_caps & HAL_AUDIO_CAP_MIXER) == 0) {
        return HAL_ERR_UNSUPPORTED;
    }
    return g_ops->set_muted(in);
}
