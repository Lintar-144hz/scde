#ifndef HAL_HAL_H
#define HAL_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAL_VERSION_MAJOR 0
#define HAL_VERSION_MINOR 1

typedef enum {
    HAL_OK = 0,
    HAL_ERR_UNAVAILABLE = 1,
    HAL_ERR_UNSUPPORTED = 2,
    HAL_ERR_PERMISSION = 3,
    HAL_ERR_IO = 4,
    HAL_ERR_INVALID = 5,
    HAL_ERR_NOMEM = 6,
    HAL_ERR_BUSY = 7
} hal_status_t;

typedef enum {
    HAL_MODULE_GPU = 1u << 0,
    HAL_MODULE_BLUETOOTH = 1u << 1,
    HAL_MODULE_AUDIO = 1u << 2
} hal_module_t;

#define HAL_MODULE_ALL (HAL_MODULE_GPU | HAL_MODULE_BLUETOOTH | HAL_MODULE_AUDIO)

enum {
    HAL_INIT_FORCE_UNAVAILABLE = 1u << 0
};

typedef struct {
    uint32_t modules;
    uint32_t flags;
} hal_init_opts_t;

typedef struct {
    hal_module_t module;
    const char *name;
    const char *backend;
    bool available;
    uint32_t capabilities;
} hal_module_info_t;

const char *hal_version(void);
const char *hal_status_str(hal_status_t status);
bool hal_is_android(void);

hal_status_t hal_init(const hal_init_opts_t *opts);
void hal_shutdown(void);
bool hal_is_inited(void);

bool hal_module_available(hal_module_t module);
uint32_t hal_available_mask(void);
hal_status_t hal_module_get_info(hal_module_t module, hal_module_info_t *out);

#endif
