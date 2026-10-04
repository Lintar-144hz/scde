#ifndef HAL_BACKEND_H
#define HAL_BACKEND_H

#include <hal/audio.h>
#include <hal/bluetooth.h>
#include <hal/gpu.h>
#include <hal/hal.h>

typedef struct {
    const char *name;
    hal_status_t (*probe)(bool *available, uint32_t *capabilities);
    int (*count)(void);
    hal_status_t (*get)(int index, hal_gpu_info_t *out);
} hal_gpu_backend_t;

typedef struct {
    const char *name;
    hal_status_t (*probe)(bool *available, uint32_t *capabilities);
    int (*adapter_count)(void);
    hal_status_t (*adapter_get)(int index, hal_bt_adapter_t *out);
    hal_status_t (*set_powered)(bool on);
    hal_status_t (*get_powered)(bool *on);
    hal_status_t (*set_discovery)(bool on);
} hal_bt_backend_t;

typedef struct {
    const char *name;
    hal_status_t (*probe)(bool *available, uint32_t *capabilities);
    int (*count)(hal_audio_dir_t direction);
    hal_status_t (*get)(int index, hal_audio_dir_t direction,
                        hal_audio_device_t *out);
    hal_status_t (*get_volume)(float *out01);
    hal_status_t (*set_volume)(float in01);
    hal_status_t (*get_muted)(bool *out);
    hal_status_t (*set_muted)(bool in);
} hal_audio_backend_t;

const hal_gpu_backend_t *hal_backend_gpu(void);
const hal_bt_backend_t *hal_backend_bluetooth(void);
const hal_audio_backend_t *hal_backend_audio(void);

const hal_gpu_backend_t *hal_backend_gpu_linux(void);
const hal_bt_backend_t *hal_backend_bluetooth_linux(void);
const hal_audio_backend_t *hal_backend_audio_linux(void);

const hal_gpu_backend_t *hal_backend_gpu_android(void);
const hal_bt_backend_t *hal_backend_bluetooth_android(void);
const hal_audio_backend_t *hal_backend_audio_android(void);

const char *hal_gpu_backend_name(void);
const char *hal_bluetooth_backend_name(void);
const char *hal_audio_backend_name(void);

const hal_gpu_backend_t *hal_gpu_null_backend(void);
const hal_bt_backend_t *hal_bluetooth_null_backend(void);
const hal_audio_backend_t *hal_audio_null_backend(void);

#endif
