#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "hal_internal.h"
#include "linux_util.h"

#define GPU_MAX 8

struct gpu_entry {
    unsigned card;
    hal_gpu_info_t info;
};

static struct {
    int count;
    struct gpu_entry entries[GPU_MAX];
} g;

static const struct {
    uint16_t vendor_id;
    const char *name;
    hal_gpu_type_t type;
} vendor_table[] = {
    { 0x10de, "NVIDIA", HAL_GPU_TYPE_DISCRETE },
    { 0x8086, "Intel", HAL_GPU_TYPE_INTEGRATED },
    { 0x1002, "AMD", HAL_GPU_TYPE_UNKNOWN },
    { 0x15ad, "VMware", HAL_GPU_TYPE_VIRTUAL },
    { 0x1af4, "Virtio", HAL_GPU_TYPE_VIRTUAL },
    { 0x14e4, "Broadcom", HAL_GPU_TYPE_UNKNOWN },
    { 0x13b5, "ARM", HAL_GPU_TYPE_UNKNOWN },
    { 0x5143, "Qualcomm", HAL_GPU_TYPE_UNKNOWN },
    { 0x106b, "Apple", HAL_GPU_TYPE_UNKNOWN }
};

static const char *const virtual_drivers[] = {
    "vgem", "vkms", "virtio_gpu", "vmwgfx", "bochs", "qxl", "cirrus"
};

static const char *const software_drivers[] = {
    "llvmpipe", "softpipe", "swrast", "kms_swrast"
};

static bool driver_is_software(const char *driver)
{
    size_t i;

    for (i = 0; i < sizeof(software_drivers) / sizeof(software_drivers[0]);
         i++) {
        if (strcmp(driver, software_drivers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static hal_status_t map_open_error(int err)
{
    if (err == ENOENT || err == ENOTDIR) {
        return HAL_ERR_UNAVAILABLE;
    }
    if (err == EACCES || err == EPERM) {
        return HAL_ERR_PERMISSION;
    }
    return HAL_ERR_IO;
}

static const char *vendor_name(uint16_t id)
{
    size_t i;

    for (i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++) {
        if (vendor_table[i].vendor_id == id) {
            return vendor_table[i].name;
        }
    }
    return "unknown";
}

static hal_gpu_type_t vendor_type(uint16_t id)
{
    size_t i;

    for (i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++) {
        if (vendor_table[i].vendor_id == id) {
            return vendor_table[i].type;
        }
    }
    return HAL_GPU_TYPE_UNKNOWN;
}

static bool driver_is_virtual(const char *driver)
{
    size_t i;

    for (i = 0; i < sizeof(virtual_drivers) / sizeof(virtual_drivers[0]);
         i++) {
        if (strcmp(driver, virtual_drivers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool any_render_node_matches(const char *device_target)
{
    char dirpath[HAL_LINUX_PATH_MAX];
    DIR *dir;
    struct dirent *ent;
    bool found = false;

    if (hal_linux_path(dirpath, sizeof(dirpath), "/sys/class/drm") !=
        HAL_OK) {
        return false;
    }
    dir = opendir(dirpath);
    if (dir == NULL) {
        return false;
    }
    while ((ent = readdir(dir)) != NULL) {
        char suffix[HAL_LINUX_PATH_MAX];
        char target[HAL_LINUX_PATH_MAX];

        if (strncmp(ent->d_name, "renderD", 7) != 0) {
            continue;
        }
        snprintf(suffix, sizeof(suffix), "/sys/class/drm/%s/device",
                 ent->d_name);
        if (hal_linux_path(target, sizeof(target), suffix) != HAL_OK) {
            continue;
        }
        {
            char linkbuf[HAL_LINUX_PATH_MAX];
            ssize_t n = readlink(target, linkbuf, sizeof(linkbuf) - 1);

            if (n > 0) {
                linkbuf[n] = '\0';
                if (strcmp(linkbuf, device_target) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    closedir(dir);
    return found;
}

static void fill_device(unsigned card, hal_gpu_info_t *out)
{
    char suffix[HAL_LINUX_PATH_MAX];
    char text[128];
    uint64_t value;

    memset(out, 0, sizeof(*out));

    snprintf(suffix, sizeof(suffix), "/sys/class/drm/card%u/device/vendor",
             card);
    if (hal_linux_read(suffix, text, sizeof(text)) == HAL_OK &&
        hal_parse_u64_hex(text, &value) == HAL_OK) {
        out->vendor_id = (uint16_t)value;
    }
    snprintf(suffix, sizeof(suffix), "/sys/class/drm/card%u/device/device",
             card);
    if (hal_linux_read(suffix, text, sizeof(text)) == HAL_OK &&
        hal_parse_u64_hex(text, &value) == HAL_OK) {
        out->device_id = (uint16_t)value;
    }

    snprintf(suffix, sizeof(suffix), "/sys/class/drm/card%u/device/driver",
             card);
    if (hal_linux_readlink_basename(suffix, out->driver,
                                    sizeof(out->driver)) != HAL_OK) {
        out->driver[0] = '\0';
    }

    snprintf(suffix, sizeof(suffix), "/sys/class/drm/card%u/device", card);
    {
        char full[HAL_LINUX_PATH_MAX];
        char linkbuf[HAL_LINUX_PATH_MAX];
        ssize_t n;

        out->render_capable = false;
        if (hal_linux_path(full, sizeof(full), suffix) == HAL_OK) {
            n = readlink(full, linkbuf, sizeof(linkbuf) - 1);
            if (n > 0) {
                linkbuf[n] = '\0';
                out->render_capable =
                    any_render_node_matches(linkbuf);
            }
        }
    }

    snprintf(suffix, sizeof(suffix), "/dev/dri/card%u", card);
    out->modeset_capable = hal_linux_exists(suffix);

    snprintf(suffix, sizeof(suffix),
             "/sys/class/drm/card%u/device/mem_info_vram_size", card);
    if (hal_linux_read(suffix, text, sizeof(text)) == HAL_OK &&
        hal_parse_u64_hex(text, &value) == HAL_OK) {
        out->vram_bytes = value;
    }

    out->type = vendor_type(out->vendor_id);
    if (out->driver[0] != '\0') {
        if (driver_is_virtual(out->driver)) {
            out->type = HAL_GPU_TYPE_VIRTUAL;
        } else if (driver_is_software(out->driver)) {
            out->type = HAL_GPU_TYPE_SOFTWARE;
        }
        (void)hal_strlcpy(out->name, out->driver, sizeof(out->name));
    } else {
        snprintf(out->name, sizeof(out->name), "card%u", card);
    }
    if (out->name[0] == '\0') {
        snprintf(out->name, sizeof(out->name), "card%u", card);
    }
    (void)hal_strlcpy(out->vendor, vendor_name(out->vendor_id),
                      sizeof(out->vendor));
}

static void sort_entries(void)
{
    int i;

    for (i = 1; i < g.count; i++) {
        struct gpu_entry tmp = g.entries[i];
        int j = i - 1;

        while (j >= 0 && g.entries[j].card > tmp.card) {
            g.entries[j + 1] = g.entries[j];
            j--;
        }
        g.entries[j + 1] = tmp;
    }
}

static hal_status_t gpu_probe(bool *available, uint32_t *capabilities)
{
    char dirpath[HAL_LINUX_PATH_MAX];
    DIR *dir;
    struct dirent *ent;
    uint32_t caps = 0;

    if (available != NULL) {
        *available = false;
    }
    if (capabilities != NULL) {
        *capabilities = 0;
    }
    g.count = 0;

    if (hal_linux_path(dirpath, sizeof(dirpath), "/sys/class/drm") !=
        HAL_OK) {
        return HAL_ERR_UNAVAILABLE;
    }
    dir = opendir(dirpath);
    if (dir == NULL) {
        return map_open_error(errno);
    }
    while ((ent = readdir(dir)) != NULL && g.count < GPU_MAX) {
        unsigned long card = 0;

        if (!hal_name_has_prefix(ent->d_name, "card", &card)) {
            continue;
        }
        g.entries[g.count].card = (unsigned)card;
        fill_device((unsigned)card, &g.entries[g.count].info);
        g.count++;
    }
    closedir(dir);

    if (g.count == 0) {
        return HAL_ERR_UNAVAILABLE;
    }
    sort_entries();
    {
        int i;

        for (i = 0; i < g.count; i++) {
            if (g.entries[i].info.render_capable) {
                caps |= HAL_GPU_CAP_RENDER;
            }
            if (g.entries[i].info.modeset_capable) {
                caps |= HAL_GPU_CAP_MODESET;
            }
            if (g.entries[i].info.type == HAL_GPU_TYPE_SOFTWARE ||
                g.entries[i].info.type == HAL_GPU_TYPE_VIRTUAL) {
                caps |= HAL_GPU_CAP_SOFTWARE;
            }
        }
    }
    if (available != NULL) {
        *available = true;
    }
    if (capabilities != NULL) {
        *capabilities = caps;
    }
    return HAL_OK;
}

static int gpu_count(void)
{
    return g.count;
}

static hal_status_t gpu_get(int index, hal_gpu_info_t *out)
{
    if (index < 0 || index >= g.count || out == NULL) {
        return HAL_ERR_INVALID;
    }
    *out = g.entries[index].info;
    return HAL_OK;
}

static const hal_gpu_backend_t linux_backend = {
    "linux-drm",
    gpu_probe,
    gpu_count,
    gpu_get
};

const hal_gpu_backend_t *hal_backend_gpu_linux(void)
{
    return &linux_backend;
}
