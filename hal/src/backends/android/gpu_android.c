#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "hal_internal.h"
#include "backends/linux/linux_util.h"

#define MODE_NONE 0
#define MODE_KGSL 1
#define MODE_MALI 2
#define MODE_DRM 3

static struct {
    int mode;
    hal_status_t error;
    hal_gpu_info_t node_info;
} g;

static hal_status_t open_gpu_node(const char *suffix)
{
    char path[HAL_LINUX_PATH_MAX];
    int fd;

    if (hal_linux_path(path, sizeof(path), suffix) != HAL_OK) {
        return HAL_ERR_UNAVAILABLE;
    }
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return HAL_OK;
    }
    if (errno == EACCES || errno == EPERM) {
        return HAL_ERR_PERMISSION;
    }
    if (errno == ENOENT) {
        return HAL_ERR_UNAVAILABLE;
    }
    return HAL_ERR_IO;
}

static void fill_node_info(const char *name, const char *vendor,
                           const char *driver)
{
    memset(&g.node_info, 0, sizeof(g.node_info));
    (void)hal_strlcpy(g.node_info.name, name, sizeof(g.node_info.name));
    (void)hal_strlcpy(g.node_info.vendor, vendor, sizeof(g.node_info.vendor));
    (void)hal_strlcpy(g.node_info.driver, driver,
                      sizeof(g.node_info.driver));
    g.node_info.type = HAL_GPU_TYPE_INTEGRATED;
    g.node_info.render_capable = true;
    g.node_info.modeset_capable = false;
}

static hal_status_t android_gpu_probe(bool *available, uint32_t *capabilities)
{
    const hal_gpu_backend_t *lin = hal_backend_gpu_linux();
    hal_status_t st;

    if (available != NULL) {
        *available = false;
    }
    if (capabilities != NULL) {
        *capabilities = 0;
    }
    g.mode = MODE_NONE;
    g.error = HAL_ERR_UNAVAILABLE;

    st = open_gpu_node("/dev/kgsl-3d0");
    if (st == HAL_OK) {
        fill_node_info("Adreno (kgsl-3d0)", "Qualcomm", "kgsl");
        g.mode = MODE_KGSL;
        if (available != NULL) {
            *available = true;
        }
        if (capabilities != NULL) {
            *capabilities = HAL_GPU_CAP_RENDER;
        }
        return HAL_OK;
    }
    g.error = st;

    st = open_gpu_node("/dev/mali0");
    if (st != HAL_OK) {
        st = open_gpu_node("/dev/mali");
    }
    if (st == HAL_OK) {
        fill_node_info("Mali", "ARM", "mali");
        g.mode = MODE_MALI;
        if (available != NULL) {
            *available = true;
        }
        if (capabilities != NULL) {
            *capabilities = HAL_GPU_CAP_RENDER;
        }
        return HAL_OK;
    }
    if (st != HAL_ERR_UNAVAILABLE) {
        g.error = st;
    }

    st = lin->probe(available, capabilities);
    if (available != NULL && *available) {
        g.mode = MODE_DRM;
        return st;
    }
    return g.error != HAL_ERR_UNAVAILABLE ? g.error : st;
}

static int android_gpu_count(void)
{
    const hal_gpu_backend_t *lin = hal_backend_gpu_linux();

    if (g.mode == MODE_DRM) {
        return lin->count();
    }
    if (g.mode == MODE_NONE) {
        return 0;
    }
    return 1;
}

static hal_status_t android_gpu_get(int index, hal_gpu_info_t *out)
{
    const hal_gpu_backend_t *lin = hal_backend_gpu_linux();

    if (g.mode == MODE_DRM) {
        return lin->get(index, out);
    }
    if (g.mode == MODE_NONE || out == NULL) {
        return HAL_ERR_UNAVAILABLE;
    }
    if (index != 0) {
        return HAL_ERR_INVALID;
    }
    *out = g.node_info;
    return HAL_OK;
}

static const hal_gpu_backend_t android_backend = {
    "android",
    android_gpu_probe,
    android_gpu_count,
    android_gpu_get
};

const hal_gpu_backend_t *hal_backend_gpu_android(void)
{
    return &android_backend;
}
