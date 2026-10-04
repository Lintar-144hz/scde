#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend.h"
#include "hal_internal.h"
#include "backends/linux/linux_util.h"

static bool g_pulse;

static bool pulse_unix(const char *path)
{
    struct sockaddr_un addr;
    int fd;
    bool ok;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        return false;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)hal_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
    ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static bool pulse_tcp(const char *host, unsigned int port)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    int fd;
    int flags;
    int err = 0;
    socklen_t errlen = sizeof(err);
    bool ok = false;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return false;
    }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        close(fd);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 250) > 0 &&
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0) {
        ok = err == 0;
    }
    close(fd);
    return ok;
}

static bool pulse_alive(void)
{
    const char *srv = getenv("PULSE_SERVER");
    char host[128];
    unsigned int port = 4713;
    const char *colon;
    size_t len;

    if (srv == NULL || srv[0] == '\0') {
        return false;
    }
    if (srv[0] == '/') {
        return pulse_unix(srv);
    }
    if (strncmp(srv, "unix:", 5) == 0) {
        return pulse_unix(srv + 5);
    }
    if (strncmp(srv, "tcp4:", 5) == 0) {
        srv += 5;
    } else if (strncmp(srv, "tcp:", 4) == 0) {
        srv += 4;
    }
    colon = strrchr(srv, ':');
    if (colon != NULL && colon != srv) {
        char *end = NULL;
        unsigned long parsed = strtoul(colon + 1, &end, 10);

        if (end != NULL && *end == '\0' && parsed > 0 && parsed < 65536) {
            port = (unsigned int)parsed;
            len = (size_t)(colon - srv);
        } else {
            len = strlen(srv);
        }
    } else {
        len = strlen(srv);
    }
    if (len == 0 || len >= sizeof(host)) {
        return false;
    }
    memcpy(host, srv, len);
    host[len] = '\0';
    return pulse_tcp(host, port);
}

static hal_status_t android_audio_probe(bool *available,
                                        uint32_t *capabilities)
{
    const hal_audio_backend_t *lin = hal_backend_audio_linux();
    hal_status_t st = lin->probe(available, capabilities);

    g_pulse = pulse_alive();
    if (available != NULL && *available) {
        if (g_pulse && capabilities != NULL) {
            *capabilities |= HAL_AUDIO_CAP_PULSE;
        }
        return st;
    }
    if (!g_pulse) {
        return st;
    }
    if (available != NULL) {
        *available = true;
    }
    if (capabilities != NULL) {
        *capabilities = HAL_AUDIO_CAP_PULSE;
    }
    return HAL_OK;
}

static int android_audio_count(hal_audio_dir_t direction)
{
    return hal_backend_audio_linux()->count(direction);
}

static hal_status_t android_audio_get(int index, hal_audio_dir_t direction,
                                      hal_audio_device_t *out)
{
    return hal_backend_audio_linux()->get(index, direction, out);
}

static hal_status_t android_audio_get_volume(float *out01)
{
    return hal_backend_audio_linux()->get_volume(out01);
}

static hal_status_t android_audio_set_volume(float in01)
{
    return hal_backend_audio_linux()->set_volume(in01);
}

static hal_status_t android_audio_get_muted(bool *out)
{
    return hal_backend_audio_linux()->get_muted(out);
}

static hal_status_t android_audio_set_muted(bool in)
{
    return hal_backend_audio_linux()->set_muted(in);
}

static const hal_audio_backend_t android_backend = {
    "android",
    android_audio_probe,
    android_audio_count,
    android_audio_get,
    android_audio_get_volume,
    android_audio_set_volume,
    android_audio_get_muted,
    android_audio_set_muted
};

const hal_audio_backend_t *hal_backend_audio_android(void)
{
    return &android_backend;
}
