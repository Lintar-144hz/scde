#ifndef HAL_GPU_H
#define HAL_GPU_H

#include <hal/hal.h>

typedef enum {
    HAL_GPU_TYPE_UNKNOWN = 0,
    HAL_GPU_TYPE_SOFTWARE,
    HAL_GPU_TYPE_INTEGRATED,
    HAL_GPU_TYPE_DISCRETE,
    HAL_GPU_TYPE_VIRTUAL
} hal_gpu_type_t;

typedef struct {
    char name[64];
    char vendor[48];
    char driver[48];
    uint16_t vendor_id;
    uint16_t device_id;
    hal_gpu_type_t type;
    bool render_capable;
    bool modeset_capable;
    uint64_t vram_bytes;
} hal_gpu_info_t;

#define HAL_GPU_CAP_RENDER (1u << 0)
#define HAL_GPU_CAP_MODESET (1u << 1)
#define HAL_GPU_CAP_SOFTWARE (1u << 2)

uint32_t hal_gpu_capabilities(void);
bool hal_gpu_available(void);
int hal_gpu_count(void);
hal_status_t hal_gpu_get_info(int index, hal_gpu_info_t *out);
hal_status_t hal_gpu_primary(hal_gpu_info_t *out);

#endif
