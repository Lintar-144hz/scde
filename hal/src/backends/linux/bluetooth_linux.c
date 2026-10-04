#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backend.h"
#include "hal_internal.h"
#include "linux_util.h"

#define BT_MAX 8
#define RFKILL_MAX 16

struct rf_entry {
    uint32_t idx;
    bool present;
    bool soft;
};

static struct {
    int count;
    uint32_t hci[BT_MAX];
    uint32_t rfkill_idx[BT_MAX];
    bool rfkill_ok;
    struct rf_entry rf[RFKILL_MAX];
    int rf_count;
} g;

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

static struct rf_entry *rf_find(uint32_t idx)
{
    int i;

    for (i = 0; i < g.rf_count; i++) {
        if (g.rf[i].idx == idx) {
            return &g.rf[i];
        }
    }
    return NULL;
}

static void rf_upsert(uint32_t idx, bool soft)
{
    struct rf_entry *e = rf_find(idx);

    if (e == NULL) {
        if (g.rf_count >= RFKILL_MAX) {
            return;
        }
        e = &g.rf[g.rf_count++];
        e->idx = idx;
    }
    e->present = true;
    e->soft = soft;
}

static void rfkill_refresh(void)
{
    char path[HAL_LINUX_PATH_MAX];
    int fd;
    struct rfkill_event ev;

    if (hal_linux_path(path, sizeof(path), "/dev/rfkill") != HAL_OK) {
        g.rfkill_ok = false;
        return;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        g.rfkill_ok = false;
        return;
    }
    g.rfkill_ok = true;
    for (;;) {
        ssize_t n = read(fd, &ev, sizeof(ev));

        if (n != (ssize_t)sizeof(ev)) {
            break;
        }
        if (ev.type == RFKILL_TYPE_BLUETOOTH) {
            rf_upsert(ev.idx, ev.soft != 0);
        }
    }
    close(fd);
}

static void resolve_rfkill_indexes(void)
{
    int i;

    for (i = 0; i < g.count; i++) {
        char suffix[HAL_LINUX_PATH_MAX];
        char dirpath[HAL_LINUX_PATH_MAX];
        DIR *dir;
        struct dirent *ent;

        g.rfkill_idx[i] = g.hci[i];
        snprintf(suffix, sizeof(suffix), "/sys/class/bluetooth/hci%u",
                 (unsigned)g.hci[i]);
        if (hal_linux_path(dirpath, sizeof(dirpath), suffix) != HAL_OK) {
            continue;
        }
        dir = opendir(dirpath);
        if (dir == NULL) {
            continue;
        }
        while ((ent = readdir(dir)) != NULL) {
            char idxsuffix[HAL_LINUX_PATH_MAX];
            char text[32];
            unsigned long value;
            char *end = NULL;

            if (!hal_name_has_prefix(ent->d_name, "rfkill", NULL)) {
                continue;
            }
            snprintf(idxsuffix, sizeof(idxsuffix),
                     "/sys/class/bluetooth/hci%u/%s/index",
                     (unsigned)g.hci[i], ent->d_name);
            if (hal_linux_read(idxsuffix, text, sizeof(text)) != HAL_OK) {
                continue;
            }
            value = strtoul(text, &end, 10);
            if (end == text) {
                continue;
            }
            g.rfkill_idx[i] = (uint32_t)value;
            break;
        }
        closedir(dir);
    }
}

static hal_status_t bt_probe(bool *available, uint32_t *capabilities)
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
    g.rf_count = 0;
    g.rfkill_ok = false;

    if (hal_linux_path(dirpath, sizeof(dirpath), "/sys/class/bluetooth") !=
        HAL_OK) {
        return HAL_ERR_UNAVAILABLE;
    }
    dir = opendir(dirpath);
    if (dir == NULL) {
        return map_open_error(errno);
    }
    while ((ent = readdir(dir)) != NULL && g.count < BT_MAX) {
        unsigned long num = 0;

        if (!hal_name_has_prefix(ent->d_name, "hci", &num)) {
            continue;
        }
        g.hci[g.count] = (uint32_t)num;
        g.count++;
    }
    closedir(dir);
    if (g.count == 0) {
        return HAL_ERR_UNAVAILABLE;
    }

    resolve_rfkill_indexes();
    rfkill_refresh();

    caps |= HAL_BT_CAP_ADAPTER;
    if (g.rfkill_ok) {
        caps |= HAL_BT_CAP_POWER;
    }
    if (available != NULL) {
        *available = true;
    }
    if (capabilities != NULL) {
        *capabilities = caps;
    }
    return HAL_OK;
}

static int bt_adapter_count(void)
{
    return g.count;
}

static hal_status_t bt_adapter_get(int index, hal_bt_adapter_t *out)
{
    char suffix[HAL_LINUX_PATH_MAX];
    struct rf_entry *rf;
    char text[64];

    if (index < 0 || index >= g.count || out == NULL) {
        return HAL_ERR_INVALID;
    }
    out->index = g.hci[index];
    snprintf(suffix, sizeof(suffix), "/sys/class/bluetooth/hci%u/address",
             (unsigned)g.hci[index]);
    if (hal_linux_read(suffix, text, sizeof(text)) != HAL_OK) {
        text[0] = '\0';
    }
    (void)hal_strlcpy(out->address, text, sizeof(out->address));
    snprintf(suffix, sizeof(suffix), "/sys/class/bluetooth/hci%u/name",
             (unsigned)g.hci[index]);
    if (hal_linux_read(suffix, text, sizeof(text)) != HAL_OK ||
        text[0] == '\0') {
        snprintf(text, sizeof(text), "hci%u", (unsigned)g.hci[index]);
    }
    (void)hal_strlcpy(out->name, text, sizeof(out->name));

    rf = g.rfkill_ok ? rf_find(g.rfkill_idx[index]) : NULL;
    if (rf != NULL && rf->present) {
        out->power_known = true;
        out->blocked_by_rfkill = rf->soft;
        out->powered = !rf->soft;
    } else {
        out->power_known = false;
        out->blocked_by_rfkill = false;
        out->powered = false;
    }
    return HAL_OK;
}

static hal_status_t bt_set_powered(bool on)
{
    char path[HAL_LINUX_PATH_MAX];
    int fd;
    int i;
    int written = 0;

    if (!g.rfkill_ok) {
        return HAL_ERR_UNSUPPORTED;
    }
    if (hal_linux_path(path, sizeof(path), "/dev/rfkill") != HAL_OK) {
        return HAL_ERR_UNAVAILABLE;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return map_open_error(errno);
    }
    for (i = 0; i < g.count; i++) {
        struct rfkill_event ev;

        memset(&ev, 0, sizeof(ev));
        ev.idx = g.rfkill_idx[i];
        ev.type = RFKILL_TYPE_BLUETOOTH;
        ev.op = RFKILL_OP_CHANGE;
        ev.soft = on ? 0 : 1;
        ev.hard = 0;
        if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
            int err = errno;

            close(fd);
            if (err == EPERM || err == EACCES) {
                return HAL_ERR_PERMISSION;
            }
            return HAL_ERR_IO;
        }
        written++;
    }
    close(fd);
    if (written == 0) {
        return HAL_ERR_UNAVAILABLE;
    }
    g.rf_count = 0;
    rfkill_refresh();
    return HAL_OK;
}

static hal_status_t bt_get_powered(bool *on)
{
    struct rf_entry *rf;

    if (g.count == 0 || !g.rfkill_ok) {
        return HAL_ERR_UNAVAILABLE;
    }
    rf = rf_find(g.rfkill_idx[0]);
    if (rf == NULL || !rf->present) {
        return HAL_ERR_UNSUPPORTED;
    }
    if (on != NULL) {
        *on = !rf->soft;
    }
    return HAL_OK;
}

static hal_status_t bt_set_discovery(bool on)
{
    (void)on;
    return HAL_ERR_UNSUPPORTED;
}

static const hal_bt_backend_t linux_backend = {
    "linux-bluez",
    bt_probe,
    bt_adapter_count,
    bt_adapter_get,
    bt_set_powered,
    bt_get_powered,
    bt_set_discovery
};

const hal_bt_backend_t *hal_backend_bluetooth_linux(void)
{
    return &linux_backend;
}
