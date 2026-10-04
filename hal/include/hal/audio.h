#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <hal/hal.h>

typedef enum {
    HAL_AUDIO_PLAYBACK = 0,
    HAL_AUDIO_CAPTURE = 1
} hal_audio_dir_t;

typedef struct {
    int card;
    int device;
    hal_audio_dir_t direction;
    char id[32];
    char name[64];
} hal_audio_device_t;

#define HAL_AUDIO_CAP_PLAYBACK (1u << 0)
#define HAL_AUDIO_CAP_CAPTURE (1u << 1)
#define HAL_AUDIO_CAP_MIXER (1u << 2)
#define HAL_AUDIO_CAP_PULSE (1u << 3)

uint32_t hal_audio_capabilities(void);
bool hal_audio_available(void);
int hal_audio_device_count(hal_audio_dir_t direction);
hal_status_t hal_audio_get_device(int index, hal_audio_dir_t direction,
                                  hal_audio_device_t *out);
hal_status_t hal_audio_get_master_volume(float *out01);
hal_status_t hal_audio_set_master_volume(float in01);
hal_status_t hal_audio_get_muted(bool *out);
hal_status_t hal_audio_set_muted(bool in);

#endif
