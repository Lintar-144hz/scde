#include <hal/audio.h>
#include <hal/bluetooth.h>
#include <hal/gpu.h>
#include <hal/hal.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);   \
        }                                                                     \
    } while (0)

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static void write_bin(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static void make_dirs(const char *path)
{
    char tmp[512];
    size_t i;

    if (strlen(path) >= sizeof(tmp)) {
        exit(2);
    }
    strcpy(tmp, path);
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir %s: %s\n", tmp, strerror(errno));
                exit(2);
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", tmp, strerror(errno));
        exit(2);
    }
}

static void rm_rf(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *ent;
    char child[600];

    if (dir == NULL) {
        unlink(path);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        rm_rf(child);
    }
    closedir(dir);
    rmdir(path);
}

static void build_fake_tree(const char *root)
{
    char p[512];
    char target[512];
    struct rfkill_event_bytes {
        unsigned char idx[4];
        unsigned char type;
        unsigned char op;
        unsigned char soft;
        unsigned char hard;
    } ev;

#define P(suffix) (snprintf(p, sizeof(p), "%s%s", root, suffix), p)

    make_dirs(P("/sys/devices/pci0000:00/0000:00:02.0"));
    make_dirs(P("/sys/bus/pci/drivers/nouveau"));
    make_dirs(P("/sys/class/drm/card0"));
    make_dirs(P("/sys/class/drm/card1"));
    make_dirs(P("/sys/class/drm/renderD128"));
    make_dirs(P("/sys/class/bluetooth/hci0/rfkill0"));
    make_dirs(P("/dev/snd"));
    make_dirs(P("/dev/dri"));
    make_dirs(P("/proc/asound"));

    write_file(P("/sys/devices/pci0000:00/0000:00:02.0/vendor"),
               "0x10de\n");
    write_file(P("/sys/devices/pci0000:00/0000:00:02.0/device"),
               "0x1b80\n");
    write_file(P("/sys/devices/pci0000:00/0000:00:02.0/mem_info_vram_size"),
               "8589934592\n");
    snprintf(target, sizeof(target),
             "%s/sys/bus/pci/drivers/nouveau", root);
    if (symlink(target, P("/sys/devices/pci0000:00/0000:00:02.0/driver")) !=
        0) {
        fprintf(stderr, "symlink driver: %s\n", strerror(errno));
        exit(2);
    }
    if (symlink("../../../devices/pci0000:00/0000:00:02.0",
                P("/sys/class/drm/card0/device")) != 0) {
        fprintf(stderr, "symlink card0: %s\n", strerror(errno));
        exit(2);
    }
    if (symlink("../../../devices/pci0000:00/0000:00:02.0",
                P("/sys/class/drm/renderD128/device")) != 0) {
        fprintf(stderr, "symlink render: %s\n", strerror(errno));
        exit(2);
    }
    make_dirs(P("/sys/class/drm/card1/device"));
    write_file(P("/sys/class/drm/card1/device/vendor"), "0x1234\n");
    write_file(P("/sys/class/drm/card1/device/device"), "0x5678\n");

    write_file(P("/sys/class/bluetooth/hci0/address"), "aa:bb:cc:dd:ee:ff\n");
    write_file(P("/sys/class/bluetooth/hci0/name"), "TestAdapter\n");
    write_file(P("/sys/class/bluetooth/hci0/rfkill0/index"), "3\n");

    memset(&ev, 0, sizeof(ev));
    ev.idx[0] = 3;
    ev.type = 2;
    ev.op = 0;
    ev.soft = 0;
    ev.hard = 0;
    write_bin(P("/dev/rfkill"), &ev, sizeof(ev));

    write_file(P("/dev/snd/controlC0"), "");
    write_file(P("/dev/snd/pcmC0D0p"), "");
    write_file(P("/dev/snd/pcmC0D1p"), "");
    write_file(P("/dev/snd/pcmC0D0c"), "");
    write_file(P("/dev/dri/card0"), "");
    write_file(P("/proc/asound/cards"),
               " 0 [PCH            ]: HDA-Intel - HDA Intel PCH\n");

#undef P
}

static void build_android_tree(const char *root)
{
    char p[512];
    struct rfkill_event_bytes {
        unsigned char idx[4];
        unsigned char type;
        unsigned char op;
        unsigned char soft;
        unsigned char hard;
    } ev;

#define Q(suffix) (snprintf(p, sizeof(p), "%s%s", root, suffix), p)

    make_dirs(Q("/sys/class/bluetooth/hci0"));
    make_dirs(Q("/dev"));
    make_dirs(Q("/system"));

    write_file(Q("/system/build.prop"), "ro.build.version.sdk=34\n");
    write_file(Q("/dev/kgsl-3d0"), "");
    write_file(Q("/sys/class/bluetooth/hci0/address"), "11:22:33:44:55:66\n");

    memset(&ev, 0, sizeof(ev));
    ev.idx[0] = 0;
    ev.type = 2;
    ev.op = 0;
    ev.soft = 0;
    ev.hard = 0;
    write_bin(Q("/dev/rfkill"), &ev, sizeof(ev));

#undef Q
}

static void test_android_tree(const char *root)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;
    hal_gpu_info_t gpu;
    hal_bt_adapter_t bt;
    bool flag;

    setenv("HAL_ROOT", root, 1);
    unsetenv("PULSE_SERVER");
    CHECK(hal_is_android(), "android detected on second tree");

    opts.modules = HAL_MODULE_ALL;
    opts.flags = 0;
    CHECK(hal_init(&opts) == HAL_OK, "init android tree");
    CHECK(hal_available_mask() == (HAL_MODULE_GPU | HAL_MODULE_BLUETOOTH),
          "gpu+bt available, audio not");

    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_OK &&
              strcmp(mi.backend, "android") == 0,
          "android gpu backend");
    CHECK(mi.capabilities == HAL_GPU_CAP_RENDER, "kgsl caps render only");
    CHECK(hal_gpu_count() == 1, "kgsl count 1");
    CHECK(hal_gpu_primary(&gpu) == HAL_OK, "kgsl primary ok");
    CHECK(strcmp(gpu.name, "Adreno (kgsl-3d0)") == 0, "kgsl name");
    CHECK(strcmp(gpu.vendor, "Qualcomm") == 0, "kgsl vendor");
    CHECK(strcmp(gpu.driver, "kgsl") == 0, "kgsl driver");
    CHECK(gpu.render_capable, "kgsl render capable");
    CHECK(!gpu.modeset_capable, "kgsl no modeset");

    CHECK(hal_module_get_info(HAL_MODULE_BLUETOOTH, &mi) == HAL_OK &&
              strcmp(mi.backend, "android") == 0,
          "android bt backend");
    CHECK(hal_bluetooth_adapter_count() == 1, "android bt count 1");
    CHECK(hal_bluetooth_get_adapter(0, &bt) == HAL_OK, "android bt ok");
    CHECK(strcmp(bt.address, "11:22:33:44:55:66") == 0,
          "android bt address");
    CHECK(hal_bluetooth_get_powered(&flag) == HAL_OK && flag,
          "android bt powered");
    CHECK(hal_bluetooth_set_powered(false) == HAL_OK,
          "android bt power off");

    CHECK(hal_module_get_info(HAL_MODULE_AUDIO, &mi) == HAL_OK &&
              strcmp(mi.backend, "android") == 0,
          "android audio backend");
    CHECK(!hal_audio_available(), "audio unavailable w/o alsa+pulse");
    CHECK(hal_audio_capabilities() == 0, "audio caps zero");
    CHECK(hal_audio_set_master_volume(0.5f) == HAL_ERR_UNAVAILABLE,
          "audio set volume unavailable");

    hal_shutdown();
}

static int pulse_listen(unsigned int *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 1) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    if (port_out != NULL) {
        *port_out = (unsigned int)ntohs(addr.sin_port);
    }
    return fd;
}

static void test_pulse(const char *root)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;
    hal_audio_device_t dev;
    float vol;
    char server[64];
    int lfd;
    unsigned int port = 0;

    setenv("HAL_ROOT", root, 1);
    lfd = pulse_listen(&port);
    CHECK(lfd >= 0, "pulse listener up");
    if (lfd < 0) {
        return;
    }
    snprintf(server, sizeof(server), "127.0.0.1:%u", port);
    setenv("PULSE_SERVER", server, 1);

    opts.modules = HAL_MODULE_AUDIO;
    opts.flags = 0;
    CHECK(hal_init(&opts) == HAL_OK, "init pulse audio");
    CHECK(hal_audio_available(), "pulse audio available");
    CHECK(hal_audio_capabilities() == HAL_AUDIO_CAP_PULSE,
          "pulse caps pulse only");
    CHECK(hal_audio_device_count(HAL_AUDIO_PLAYBACK) == 0,
          "pulse playback count 0");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_PLAYBACK, &dev) ==
              HAL_ERR_INVALID,
          "pulse device 0 invalid");
    CHECK(hal_audio_get_master_volume(&vol) == HAL_ERR_UNSUPPORTED,
          "pulse volume unsupported");
    CHECK(hal_audio_set_master_volume(0.5f) == HAL_ERR_UNSUPPORTED,
          "pulse set volume unsupported");
    CHECK(hal_module_get_info(HAL_MODULE_AUDIO, &mi) == HAL_OK,
          "pulse module info");
    CHECK(mi.available && mi.capabilities == HAL_AUDIO_CAP_PULSE,
          "pulse module info caps");
    hal_shutdown();
    close(lfd);

    setenv("PULSE_SERVER", "127.0.0.1:1", 1);
    CHECK(hal_init(&opts) == HAL_OK, "init closed pulse");
    CHECK(!hal_audio_available(), "closed pulse unavailable");
    hal_shutdown();

    unsetenv("PULSE_SERVER");
    CHECK(hal_init(&opts) == HAL_OK, "init no pulse");
    CHECK(!hal_audio_available(), "no pulse unavailable");
    hal_shutdown();
}

static void test_before_init(void)
{
    hal_gpu_info_t gpu;
    hal_bt_adapter_t bt;
    hal_module_info_t mi;
    float vol;
    bool flag;

    CHECK(!hal_is_inited(), "not inited before hal_init");
    CHECK(!hal_gpu_available(), "gpu unavailable before init");
    CHECK(hal_gpu_count() == 0, "gpu count 0 before init");
    CHECK(hal_gpu_get_info(0, &gpu) == HAL_ERR_INVALID,
          "gpu get_info before init");
    CHECK(hal_bluetooth_get_powered(&flag) == HAL_ERR_INVALID,
          "bt powered before init");
    CHECK(hal_bluetooth_get_adapter(0, &bt) == HAL_ERR_INVALID,
          "bt adapter before init");
    CHECK(hal_audio_get_master_volume(&vol) == HAL_ERR_INVALID,
          "audio volume before init");
    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_ERR_INVALID,
          "module info before init");
    hal_shutdown();
    CHECK(!hal_is_inited(), "shutdown before init is a no-op");
}

static void test_null_env(void)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;
    hal_gpu_info_t gpu;
    hal_bt_adapter_t bt;
    hal_audio_device_t dev;
    float vol;
    bool flag;

    opts.modules = HAL_MODULE_ALL;
    opts.flags = HAL_INIT_FORCE_UNAVAILABLE;
    CHECK(hal_init(&opts) == HAL_OK, "init with force flag");
    CHECK(hal_init(&opts) == HAL_ERR_BUSY, "double init returns busy");
    CHECK(hal_available_mask() == 0, "force flag hides all modules");
    CHECK(!hal_gpu_available(), "gpu forced unavailable");
    CHECK(hal_gpu_count() == 0, "gpu forced count 0");
    CHECK(hal_gpu_primary(&gpu) == HAL_ERR_UNAVAILABLE,
          "gpu primary forced unavailable");
    CHECK(!hal_bluetooth_available(), "bt forced unavailable");
    CHECK(hal_bluetooth_adapter_count() == 0, "bt forced count 0");
    CHECK(hal_bluetooth_get_adapter(0, &bt) == HAL_ERR_UNAVAILABLE,
          "bt adapter forced unavailable");
    CHECK(hal_bluetooth_set_powered(true) == HAL_ERR_UNAVAILABLE,
          "bt power set forced unavailable");
    CHECK(hal_bluetooth_get_powered(&flag) == HAL_ERR_UNAVAILABLE,
          "bt power get forced unavailable");
    CHECK(hal_bluetooth_set_discovery(true) == HAL_ERR_UNAVAILABLE,
          "bt discovery forced unavailable");
    CHECK(!hal_audio_available(), "audio forced unavailable");
    CHECK(hal_audio_device_count(HAL_AUDIO_PLAYBACK) == 0,
          "audio forced count 0");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_PLAYBACK, &dev) ==
              HAL_ERR_UNAVAILABLE,
          "audio device forced unavailable");
    CHECK(hal_audio_get_master_volume(&vol) == HAL_ERR_UNAVAILABLE,
          "audio volume forced unavailable");
    CHECK(hal_audio_set_master_volume(0.5f) == HAL_ERR_UNAVAILABLE,
          "audio volume set forced unavailable");
    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_OK,
          "module info still works");
    CHECK(strcmp(mi.name, "gpu") == 0, "gpu module name");
    CHECK(strcmp(mi.backend, "null") == 0, "null backend name");
    CHECK(!mi.available, "module info unavailable");
    CHECK(mi.capabilities == 0, "module caps zero");
    CHECK(hal_module_get_info(HAL_MODULE_BLUETOOTH, &mi) == HAL_OK &&
              strcmp(mi.name, "bluetooth") == 0,
          "bluetooth module name");
    CHECK(hal_module_get_info(HAL_MODULE_AUDIO, &mi) == HAL_OK &&
              strcmp(mi.name, "audio") == 0,
          "audio module name");
    hal_shutdown();
    CHECK(!hal_is_inited(), "shutdown clears state");
    hal_shutdown();
}

static void test_subset_init(void)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;

    opts.modules = HAL_MODULE_AUDIO;
    opts.flags = HAL_INIT_FORCE_UNAVAILABLE;
    CHECK(hal_init(&opts) == HAL_OK, "init audio only");
    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_ERR_INVALID,
          "gpu not selected -> invalid");
    CHECK(hal_module_get_info(HAL_MODULE_AUDIO, &mi) == HAL_OK,
          "audio selected -> ok");
    CHECK(!hal_gpu_available(), "gpu module disabled");
    CHECK(!hal_audio_available(), "audio forced unavailable");
    hal_shutdown();
}

static void test_fake_tree(const char *root)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;
    hal_gpu_info_t gpu;
    hal_bt_adapter_t bt;
    hal_audio_device_t dev;
    float vol;
    bool flag;
    int fd;
    unsigned char buf[8];
    ssize_t n;

    setenv("HAL_ROOT", root, 1);
    opts.modules = HAL_MODULE_ALL;
    opts.flags = 0;
    CHECK(hal_init(&opts) == HAL_OK, "init with fake tree");
    CHECK(hal_available_mask() == HAL_MODULE_ALL,
          "all modules available in fake tree");

    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_OK,
          "gpu module info");
    CHECK(strcmp(mi.backend, "linux-drm") == 0, "gpu backend name");
    CHECK(mi.available, "gpu available");
    CHECK(mi.capabilities == (HAL_GPU_CAP_RENDER | HAL_GPU_CAP_MODESET),
          "gpu caps render+modeset");
    CHECK(hal_gpu_count() == 2, "gpu count 2");
    CHECK(hal_gpu_primary(&gpu) == HAL_OK, "gpu primary ok");
    CHECK(gpu.vendor_id == 0x10de, "gpu0 vendor id");
    CHECK(strcmp(gpu.vendor, "NVIDIA") == 0, "gpu0 vendor name");
    CHECK(strcmp(gpu.driver, "nouveau") == 0, "gpu0 driver");
    CHECK(gpu.render_capable, "gpu0 render capable");
    CHECK(gpu.modeset_capable, "gpu0 modeset capable");
    CHECK(gpu.vram_bytes == 8589934592ull, "gpu0 vram");
    CHECK(gpu.type == HAL_GPU_TYPE_DISCRETE, "gpu0 type discrete");
    CHECK(hal_gpu_get_info(1, &gpu) == HAL_OK, "gpu1 info ok");
    CHECK(gpu.vendor_id == 0x1234, "gpu1 vendor id");
    CHECK(strcmp(gpu.vendor, "unknown") == 0, "gpu1 vendor name");
    CHECK(!gpu.render_capable, "gpu1 no render node");
    CHECK(!gpu.modeset_capable, "gpu1 no modeset node");
    CHECK(hal_gpu_get_info(2, &gpu) == HAL_ERR_INVALID, "gpu2 invalid");
    CHECK(hal_gpu_get_info(-1, &gpu) == HAL_ERR_INVALID, "gpu-1 invalid");
    CHECK(hal_gpu_get_info(0, NULL) == HAL_ERR_INVALID, "gpu null out");

    CHECK(hal_module_get_info(HAL_MODULE_BLUETOOTH, &mi) == HAL_OK,
          "bt module info");
    CHECK(strcmp(mi.backend, "linux-bluez") == 0, "bt backend name");
    CHECK(mi.capabilities == (HAL_BT_CAP_ADAPTER | HAL_BT_CAP_POWER),
          "bt caps adapter+power");
    CHECK(hal_bluetooth_adapter_count() == 1, "bt adapter count 1");
    CHECK(hal_bluetooth_get_adapter(0, &bt) == HAL_OK, "bt adapter ok");
    CHECK(bt.index == 0, "bt hci index");
    CHECK(strcmp(bt.address, "aa:bb:cc:dd:ee:ff") == 0, "bt address");
    CHECK(strcmp(bt.name, "TestAdapter") == 0, "bt name");
    CHECK(bt.power_known, "bt power known");
    CHECK(bt.powered, "bt initially powered");
    CHECK(hal_bluetooth_get_powered(&flag) == HAL_OK && flag,
          "bt get powered true");
    CHECK(hal_bluetooth_get_adapter(1, &bt) == HAL_ERR_INVALID,
          "bt adapter 1 invalid");
    CHECK(hal_bluetooth_get_adapter(0, NULL) == HAL_ERR_INVALID,
          "bt null out");
    CHECK(hal_bluetooth_set_powered(false) == HAL_OK, "bt power off");
    CHECK(hal_bluetooth_get_powered(&flag) == HAL_OK && !flag,
          "bt powered false after off");
    {
        char rfkill_path[600];

        snprintf(rfkill_path, sizeof(rfkill_path), "%s/dev/rfkill", root);
        fd = open(rfkill_path, O_RDONLY);
        CHECK(fd >= 0, "rfkill file readable");
        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf));
            CHECK(n == 8, "rfkill event size");
            CHECK(buf[4] == 2 && buf[5] == 2 && buf[6] == 1,
                  "rfkill change event written");
            close(fd);
        }
    }
    CHECK(hal_bluetooth_set_powered(true) == HAL_OK, "bt power on");
    CHECK(hal_bluetooth_set_discovery(true) == HAL_ERR_UNSUPPORTED,
          "bt discovery unsupported");

    CHECK(hal_module_get_info(HAL_MODULE_AUDIO, &mi) == HAL_OK,
          "audio module info");
    CHECK(strcmp(mi.backend, "linux-alsa") == 0, "audio backend name");
    CHECK(mi.capabilities ==
              (HAL_AUDIO_CAP_PLAYBACK | HAL_AUDIO_CAP_CAPTURE |
               HAL_AUDIO_CAP_MIXER),
          "audio caps all");
    CHECK(hal_audio_device_count(HAL_AUDIO_PLAYBACK) == 2,
          "playback count 2");
    CHECK(hal_audio_device_count(HAL_AUDIO_CAPTURE) == 1,
          "capture count 1");
    CHECK(hal_audio_device_count(HAL_AUDIO_CAPTURE + 99) == 0,
          "bad direction count 0");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_PLAYBACK, &dev) == HAL_OK,
          "playback device 0");
    CHECK(strcmp(dev.id, "hw:0,0") == 0, "device id");
    CHECK(strcmp(dev.name, "HDA Intel PCH") == 0, "device name from cards");
    CHECK(dev.direction == HAL_AUDIO_PLAYBACK, "device direction");
    CHECK(hal_audio_get_device(2, HAL_AUDIO_PLAYBACK, &dev) ==
              HAL_ERR_INVALID,
          "playback device 2 invalid");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_CAPTURE, &dev) == HAL_OK,
          "capture device 0");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_PLAYBACK, NULL) ==
              HAL_ERR_INVALID,
          "audio null out");
    CHECK(hal_audio_get_master_volume(&vol) == HAL_ERR_UNSUPPORTED,
          "fake mixer file -> unsupported");
    CHECK(hal_audio_set_master_volume(0.5f) == HAL_ERR_UNSUPPORTED,
          "fake mixer set -> unsupported");
    CHECK(hal_audio_set_master_volume(2.0f) == HAL_ERR_INVALID,
          "out of range rejected first");
    CHECK(hal_audio_get_muted(&flag) == HAL_ERR_UNSUPPORTED,
          "fake mixer mute -> unsupported");
    CHECK(hal_audio_get_master_volume(NULL) == HAL_ERR_INVALID,
          "audio null volume out");

    hal_shutdown();
}

static void test_missing_root(void)
{
    hal_init_opts_t opts;
    hal_module_info_t mi;
    hal_gpu_info_t gpu;
    hal_audio_device_t dev;

    setenv("HAL_ROOT", "/nonexistent-hal-root", 1);
    opts.modules = HAL_MODULE_ALL;
    opts.flags = 0;
    CHECK(hal_init(&opts) == HAL_OK, "init with missing root");
    CHECK(hal_available_mask() == 0, "no modules with missing root");
    CHECK(hal_module_get_info(HAL_MODULE_GPU, &mi) == HAL_OK,
          "gpu info missing root");
    CHECK(!mi.available, "gpu unavailable missing root");
    CHECK(strcmp(mi.backend, "linux-drm") == 0,
          "backend name kept when unavailable");
    CHECK(hal_gpu_primary(&gpu) == HAL_ERR_UNAVAILABLE,
          "gpu primary missing root");
    CHECK(hal_audio_device_count(HAL_AUDIO_PLAYBACK) == 0,
          "audio count missing root");
    CHECK(hal_audio_get_device(0, HAL_AUDIO_PLAYBACK, &dev) ==
              HAL_ERR_UNAVAILABLE,
          "audio device missing root");
    hal_shutdown();
    unsetenv("HAL_ROOT");
}

static void test_status_strings(void)
{
    int i;
    int j;

    for (i = 0; i <= HAL_ERR_BUSY; i++) {
        const char *a = hal_status_str((hal_status_t)i);

        CHECK(a != NULL && a[0] != '\0', "status string non-empty");
        for (j = 0; j < i; j++) {
            const char *b = hal_status_str((hal_status_t)j);

            CHECK(strcmp(a, b) != 0, "status strings unique");
        }
    }
    CHECK(strcmp(hal_status_str((hal_status_t)999), "unknown") == 0,
          "status string fallback");
    CHECK(hal_version() != NULL && hal_version()[0] != '\0',
          "version string");
}

int main(void)
{
    char root[256];

    char root2[256];

    strcpy(root, "/tmp/haltestXXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    build_fake_tree(root);
    strcpy(root2, "/tmp/haltestXXXXXX");
    if (mkdtemp(root2) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    build_android_tree(root2);
    unsetenv("PULSE_SERVER");

    test_before_init();
    test_null_env();
    test_subset_init();
    test_fake_tree(root);
    unsetenv("PULSE_SERVER");
    test_android_tree(root2);
    test_pulse(root2);
    test_missing_root();
    test_status_strings();

    rm_rf(root);
    rm_rf(root2);

    printf("hal smoke: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
