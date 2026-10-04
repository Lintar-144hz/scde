#include "linux_util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hal_internal.h"

const char *hal_linux_root(void)
{
    const char *root = getenv("HAL_ROOT");

    if (root == NULL || root[0] == '\0') {
        return "";
    }
    return root;
}

hal_status_t hal_linux_path(char *out, size_t cap, const char *suffix)
{
    int n;

    if (out == NULL || cap == 0 || suffix == NULL) {
        return HAL_ERR_INVALID;
    }
    n = snprintf(out, cap, "%s%s", hal_linux_root(), suffix);
    if (n < 0 || (size_t)n >= cap) {
        return HAL_ERR_NOMEM;
    }
    return HAL_OK;
}

hal_status_t hal_linux_read(const char *suffix, char *buf, size_t cap)
{
    char path[HAL_LINUX_PATH_MAX];
    hal_status_t st = hal_linux_path(path, sizeof(path), suffix);
    FILE *f;
    size_t n;

    if (st != HAL_OK) {
        return st;
    }
    if (buf == NULL || cap == 0) {
        return HAL_ERR_INVALID;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return HAL_ERR_UNAVAILABLE;
        }
        if (errno == EACCES || errno == EPERM) {
            return HAL_ERR_PERMISSION;
        }
        return HAL_ERR_IO;
    }
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f)) {
        fclose(f);
        return HAL_ERR_IO;
    }
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    return HAL_OK;
}

hal_status_t hal_linux_readlink_basename(const char *suffix, char *buf,
                                         size_t cap)
{
    char path[HAL_LINUX_PATH_MAX];
    char target[HAL_LINUX_PATH_MAX];
    ssize_t n;
    const char *base;
    hal_status_t st = hal_linux_path(path, sizeof(path), suffix);

    if (st != HAL_OK) {
        return st;
    }
    if (buf == NULL || cap == 0) {
        return HAL_ERR_INVALID;
    }
    n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return HAL_ERR_UNAVAILABLE;
        }
        if (errno == EACCES || errno == EPERM) {
            return HAL_ERR_PERMISSION;
        }
        return HAL_ERR_IO;
    }
    target[n] = '\0';
    base = strrchr(target, '/');
    base = base != NULL ? base + 1 : target;
    if (base[0] == '\0') {
        return HAL_ERR_IO;
    }
    (void)hal_strlcpy(buf, base, cap);
    return HAL_OK;
}

bool hal_linux_exists(const char *suffix)
{
    char path[HAL_LINUX_PATH_MAX];

    if (hal_linux_path(path, sizeof(path), suffix) != HAL_OK) {
        return false;
    }
    return access(path, F_OK) == 0;
}
