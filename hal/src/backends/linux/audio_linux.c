#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#include "backend.h"
#include "hal_internal.h"
#include "linux_util.h"

#define AUDIO_MAX_DEVICES 16
#define AUDIO_MAX_CARDS 4

static struct {
    int n_playback;
    int n_capture;
    hal_audio_device_t playback[AUDIO_MAX_DEVICES];
    hal_audio_device_t capture[AUDIO_MAX_DEVICES];
    char card_name[AUDIO_MAX_CARDS][64];
    bool mixer_present;
} g;

static hal_status_t map_open_error(int err)
{
    if (err == ENOENT || err == ENOTDIR) {
        return HAL_ERR_UNAVAILABLE;
    }
    if (err == EACCES || err == EPERM) {
        return HAL_ERR_PERMISSION;
    }
    if (err == ENOTTY) {
        return HAL_ERR_IO;
    }
    return HAL_ERR_IO;
}

static void load_card_names(void)
{
    char path[HAL_LINUX_PATH_MAX];
    FILE *f;
    char line[256];

    memset(g.card_name, 0, sizeof(g.card_name));
    if (hal_linux_path(path, sizeof(path), "/proc/asound/cards") != HAL_OK) {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        int num = -1;
        char *br;
        char *name;
        size_t len;

        if (sscanf(line, " %d", &num) != 1 || num < 0 ||
            num >= AUDIO_MAX_CARDS) {
            continue;
        }
        br = strchr(line, ']');
        if (br == NULL || br[1] != ':') {
            continue;
        }
        name = br + 2;
        while (*name == ' ' || *name == '\t') {
            name++;
        }
        {
            char *sep = strstr(name, " - ");

            if (sep != NULL) {
                name = sep + 3;
            }
        }
        len = strlen(name);
        while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r')) {
            len--;
        }
        if (len == 0) {
            continue;
        }
        if (len >= sizeof(g.card_name[num])) {
            len = sizeof(g.card_name[num]) - 1;
        }
        memcpy(g.card_name[num], name, len);
        g.card_name[num][len] = '\0';
    }
    fclose(f);
}

static void add_device(int card, int device, hal_audio_dir_t dir)
{
    hal_audio_device_t *list;
    int *count;
    hal_audio_device_t *entry;

    if (dir == HAL_AUDIO_PLAYBACK) {
        list = g.playback;
        count = &g.n_playback;
    } else {
        list = g.capture;
        count = &g.n_capture;
    }
    if (*count >= AUDIO_MAX_DEVICES || card < 0 || card >= AUDIO_MAX_CARDS) {
        return;
    }
    entry = &list[*count];
    memset(entry, 0, sizeof(*entry));
    entry->card = card;
    entry->device = device;
    entry->direction = dir;
    snprintf(entry->id, sizeof(entry->id), "hw:%d,%d", card, device);
    if (g.card_name[card][0] != '\0') {
        (void)hal_strlcpy(entry->name, g.card_name[card],
                          sizeof(entry->name));
    } else {
        snprintf(entry->name, sizeof(entry->name), "card %d device %d",
                 card, device);
    }
    (*count)++;
}

static hal_status_t audio_probe(bool *available, uint32_t *capabilities)
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
    g.n_playback = 0;
    g.n_capture = 0;
    g.mixer_present = false;
    memset(g.playback, 0, sizeof(g.playback));
    memset(g.capture, 0, sizeof(g.capture));

    if (hal_linux_path(dirpath, sizeof(dirpath), "/dev/snd") != HAL_OK) {
        return HAL_ERR_UNAVAILABLE;
    }
    dir = opendir(dirpath);
    if (dir == NULL) {
        return map_open_error(errno);
    }
    load_card_names();
    while ((ent = readdir(dir)) != NULL) {
        int card = -1;
        int device = -1;
        char tail = '\0';

        if (sscanf(ent->d_name, "pcmC%dD%d%c", &card, &device, &tail) == 3 &&
            (tail == 'p' || tail == 'c') && card >= 0 && device >= 0) {
            add_device(card, device,
                       tail == 'p' ? HAL_AUDIO_PLAYBACK : HAL_AUDIO_CAPTURE);
            continue;
        }
        {
            unsigned long num = 0;

            if (hal_name_has_prefix(ent->d_name, "controlC", &num)) {
                g.mixer_present = true;
            }
        }
    }
    closedir(dir);

    if (g.n_playback == 0 && g.n_capture == 0 && !g.mixer_present) {
        return HAL_ERR_UNAVAILABLE;
    }
    if (g.n_playback > 0) {
        caps |= HAL_AUDIO_CAP_PLAYBACK;
    }
    if (g.n_capture > 0) {
        caps |= HAL_AUDIO_CAP_CAPTURE;
    }
    if (g.mixer_present) {
        caps |= HAL_AUDIO_CAP_MIXER;
    }
    if (available != NULL) {
        *available = true;
    }
    if (capabilities != NULL) {
        *capabilities = caps;
    }
    return HAL_OK;
}

static int audio_count(hal_audio_dir_t direction)
{
    if (direction == HAL_AUDIO_PLAYBACK) {
        return g.n_playback;
    }
    if (direction == HAL_AUDIO_CAPTURE) {
        return g.n_capture;
    }
    return 0;
}

static hal_status_t audio_get(int index, hal_audio_dir_t direction,
                              hal_audio_device_t *out)
{
    const hal_audio_device_t *list;
    int count;

    if (out == NULL) {
        return HAL_ERR_INVALID;
    }
    count = audio_count(direction);
    if (index < 0 || index >= count) {
        return HAL_ERR_INVALID;
    }
    list = direction == HAL_AUDIO_PLAYBACK ? g.playback : g.capture;
    *out = list[index];
    return HAL_OK;
}

static int open_control(int *fd_out)
{
    char suffix[HAL_LINUX_PATH_MAX];
    char path[HAL_LINUX_PATH_MAX];
    int c;
    int last_err = ENOENT;

    for (c = 0; c < AUDIO_MAX_CARDS; c++) {
        int fd;

        snprintf(suffix, sizeof(suffix), "/dev/snd/controlC%d", c);
        if (!hal_linux_exists(suffix)) {
            continue;
        }
        if (hal_linux_path(path, sizeof(path), suffix) != HAL_OK) {
            continue;
        }
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            last_err = errno;
            fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (fd >= 0) {
            *fd_out = fd;
            return 0;
        }
        last_err = errno;
    }
    return last_err;
}

static hal_status_t find_elem(int fd, int wanted_type,
                              struct snd_ctl_elem_id *id_out, long *min_out,
                              long *max_out)
{
    static const char *const names[] = { "Master", "PCM", "Speaker",
                                         "Headphone" };
    static const int ifaces[] = { SNDRV_CTL_ELEM_IFACE_MIXER,
                                  SNDRV_CTL_ELEM_IFACE_PCM,
                                  SNDRV_CTL_ELEM_IFACE_CARD };
    size_t ni;
    size_t nf;

    for (ni = 0; ni < sizeof(names) / sizeof(names[0]); ni++) {
        for (nf = 0; nf < sizeof(ifaces) / sizeof(ifaces[0]); nf++) {
            struct snd_ctl_elem_info info;

            memset(&info, 0, sizeof(info));
            info.id.iface = (__u32)ifaces[nf];
            (void)hal_strlcpy((char *)info.id.name, names[ni],
                              sizeof(info.id.name));
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) != 0) {
                continue;
            }
            if ((int)info.type != wanted_type) {
                continue;
            }
            *id_out = info.id;
            if (min_out != NULL) {
                *min_out = (long)info.value.integer.min;
            }
            if (max_out != NULL) {
                *max_out = (long)info.value.integer.max;
            }
            return HAL_OK;
        }
    }
    return HAL_ERR_UNSUPPORTED;
}

static hal_status_t volume_get(float *out01)
{
    struct snd_ctl_elem_id id;
    struct snd_ctl_elem_value val;
    long min = 0;
    long max = 0;
    int fd;
    int err;
    hal_status_t st;

    err = open_control(&fd);
    if (err != 0) {
        return map_open_error(err);
    }
    st = find_elem(fd, SNDRV_CTL_ELEM_TYPE_INTEGER, &id, &min, &max);
    if (st != HAL_OK) {
        close(fd);
        return st;
    }
    memset(&val, 0, sizeof(val));
    val.id = id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) != 0) {
        err = errno;
        close(fd);
        return map_open_error(err);
    }
    close(fd);
    if (max <= min) {
        *out01 = 0.0f;
        return HAL_OK;
    }
    *out01 = (float)((double)(val.value.integer.value[0] - min) /
                     (double)(max - min));
    if (*out01 < 0.0f) {
        *out01 = 0.0f;
    }
    if (*out01 > 1.0f) {
        *out01 = 1.0f;
    }
    return HAL_OK;
}

static hal_status_t volume_set(float in01)
{
    struct snd_ctl_elem_id id;
    struct snd_ctl_elem_value val;
    long min = 0;
    long max = 0;
    int fd;
    int err;
    hal_status_t st;

    err = open_control(&fd);
    if (err != 0) {
        return map_open_error(err);
    }
    st = find_elem(fd, SNDRV_CTL_ELEM_TYPE_INTEGER, &id, &min, &max);
    if (st != HAL_OK) {
        close(fd);
        return st;
    }
    memset(&val, 0, sizeof(val));
    val.id = id;
    if (max <= min) {
        val.value.integer.value[0] = min;
    } else {
        val.value.integer.value[0] =
            min + (long)((double)(max - min) * (double)in01);
    }
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) != 0) {
        err = errno;
        close(fd);
        return map_open_error(err);
    }
    close(fd);
    return HAL_OK;
}

static hal_status_t muted_get(bool *out)
{
    struct snd_ctl_elem_id id;
    struct snd_ctl_elem_value val;
    int fd;
    int err;
    hal_status_t st;

    err = open_control(&fd);
    if (err != 0) {
        return map_open_error(err);
    }
    st = find_elem(fd, SNDRV_CTL_ELEM_TYPE_BOOLEAN, &id, NULL, NULL);
    if (st != HAL_OK) {
        close(fd);
        return st;
    }
    memset(&val, 0, sizeof(val));
    val.id = id;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) != 0) {
        err = errno;
        close(fd);
        return map_open_error(err);
    }
    close(fd);
    if (out != NULL) {
        *out = val.value.integer.value[0] == 0;
    }
    return HAL_OK;
}

static hal_status_t muted_set(bool in)
{
    struct snd_ctl_elem_id id;
    struct snd_ctl_elem_value val;
    int fd;
    int err;
    hal_status_t st;

    err = open_control(&fd);
    if (err != 0) {
        return map_open_error(err);
    }
    st = find_elem(fd, SNDRV_CTL_ELEM_TYPE_BOOLEAN, &id, NULL, NULL);
    if (st != HAL_OK) {
        close(fd);
        return st;
    }
    memset(&val, 0, sizeof(val));
    val.id = id;
    val.value.integer.value[0] = in ? 0 : 1;
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) != 0) {
        err = errno;
        close(fd);
        return map_open_error(err);
    }
    close(fd);
    return HAL_OK;
}

static const hal_audio_backend_t linux_backend = {
    "linux-alsa",
    audio_probe,
    audio_count,
    audio_get,
    volume_get,
    volume_set,
    muted_get,
    muted_set
};

const hal_audio_backend_t *hal_backend_audio_linux(void)
{
    return &linux_backend;
}
