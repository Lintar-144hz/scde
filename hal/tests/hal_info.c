#include <hal/audio.h>
#include <hal/bluetooth.h>
#include <hal/gpu.h>
#include <hal/hal.h>

#include <stdio.h>

static const char *gpu_type_str(hal_gpu_type_t t)
{
    switch (t) {
    case HAL_GPU_TYPE_SOFTWARE:
        return "software";
    case HAL_GPU_TYPE_INTEGRATED:
        return "integrated";
    case HAL_GPU_TYPE_DISCRETE:
        return "discrete";
    case HAL_GPU_TYPE_VIRTUAL:
        return "virtual";
    default:
        return "unknown";
    }
}

static void print_module(hal_module_t module)
{
    hal_module_info_t info;
    hal_status_t st = hal_module_get_info(module, &info);

    if (st != HAL_OK) {
        printf("  module: %s\n", hal_status_str(st));
        return;
    }
    printf("  backend=%s available=%s caps=0x%x\n", info.backend,
           info.available ? "yes" : "no", info.capabilities);
}

int main(void)
{
    hal_init_opts_t opts;
    hal_status_t st;
    int i;

    opts.modules = HAL_MODULE_ALL;
    opts.flags = 0;
    st = hal_init(&opts);
    if (st != HAL_OK) {
        fprintf(stderr, "hal_init: %s\n", hal_status_str(st));
        return 1;
    }

    printf("HAL %s\n", hal_version());
    printf("available mask: 0x%x\n", hal_available_mask());

    printf("gpu:\n");
    print_module(HAL_MODULE_GPU);
    for (i = 0; i < hal_gpu_count(); i++) {
        hal_gpu_info_t gpu;

        if (hal_gpu_get_info(i, &gpu) != HAL_OK) {
            continue;
        }
        printf("  [%d] %s vendor=%s(0x%04x) device=0x%04x driver=%s "
               "type=%s render=%s modeset=%s vram=%llu\n",
               i, gpu.name, gpu.vendor, gpu.vendor_id, gpu.device_id,
               gpu.driver[0] != '\0' ? gpu.driver : "-",
               gpu_type_str(gpu.type), gpu.render_capable ? "yes" : "no",
               gpu.modeset_capable ? "yes" : "no",
               (unsigned long long)gpu.vram_bytes);
    }

    printf("bluetooth:\n");
    print_module(HAL_MODULE_BLUETOOTH);
    for (i = 0; i < hal_bluetooth_adapter_count(); i++) {
        hal_bt_adapter_t bt;

        if (hal_bluetooth_get_adapter(i, &bt) != HAL_OK) {
            continue;
        }
        printf("  [%d] %s addr=%s powered=%s%s blocked=%s\n", i, bt.name,
               bt.address[0] != '\0' ? bt.address : "-",
               bt.power_known ? (bt.powered ? "yes" : "no") : "unknown",
               bt.power_known ? "" : "(state unknown)",
               bt.blocked_by_rfkill ? "yes" : "no");
    }
    {
        bool on = false;
        st = hal_bluetooth_get_powered(&on);
        printf("  get_powered: %s\n", hal_status_str(st));
        st = hal_bluetooth_set_discovery(true);
        printf("  set_discovery: %s\n", hal_status_str(st));
    }

    printf("audio:\n");
    print_module(HAL_MODULE_AUDIO);
    for (i = 0; i < hal_audio_device_count(HAL_AUDIO_PLAYBACK); i++) {
        hal_audio_device_t dev;

        if (hal_audio_get_device(i, HAL_AUDIO_PLAYBACK, &dev) != HAL_OK) {
            continue;
        }
        printf("  playback [%d] %s (%s)\n", i, dev.id, dev.name);
    }
    for (i = 0; i < hal_audio_device_count(HAL_AUDIO_CAPTURE); i++) {
        hal_audio_device_t dev;

        if (hal_audio_get_device(i, HAL_AUDIO_CAPTURE, &dev) != HAL_OK) {
            continue;
        }
        printf("  capture  [%d] %s (%s)\n", i, dev.id, dev.name);
    }
    {
        float vol = 0.0f;
        bool muted = false;

        st = hal_audio_get_master_volume(&vol);
        printf("  get_master_volume: %s", hal_status_str(st));
        if (st == HAL_OK) {
            printf(" (%.0f%%)", vol * 100.0f);
        }
        printf("\n");
        st = hal_audio_get_muted(&muted);
        printf("  get_muted: %s\n", hal_status_str(st));
    }

    hal_shutdown();
    return 0;
}
