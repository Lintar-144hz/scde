#ifndef HAL_INTERNAL_H
#define HAL_INTERNAL_H

#include <hal/hal.h>

#define HAL_PATH_MAX 512

bool hal_force_unavailable(void);
bool hal_check_init(void);
bool hal_module_enabled(hal_module_t module);
size_t hal_strlcpy(char *dst, const char *src, size_t cap);
hal_status_t hal_read_text_file(const char *path, char *buf, size_t cap);
hal_status_t hal_parse_u64_hex(const char *text, uint64_t *out);
bool hal_name_has_prefix(const char *name, const char *prefix,
                         unsigned long *number_out);

void hal_gpu_module_init(void);
void hal_gpu_module_reset(void);
void hal_bluetooth_module_init(void);
void hal_bluetooth_module_reset(void);
void hal_audio_module_init(void);
void hal_audio_module_reset(void);

#endif
