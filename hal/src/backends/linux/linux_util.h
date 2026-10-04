#ifndef HAL_LINUX_UTIL_H
#define HAL_LINUX_UTIL_H

#include <hal/hal.h>

#define HAL_LINUX_PATH_MAX 512

const char *hal_linux_root(void);
hal_status_t hal_linux_path(char *out, size_t cap, const char *suffix);
hal_status_t hal_linux_read(const char *suffix, char *buf, size_t cap);
hal_status_t hal_linux_readlink_basename(const char *suffix, char *buf,
                                         size_t cap);
bool hal_linux_exists(const char *suffix);

#endif
