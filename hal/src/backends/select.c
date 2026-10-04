#include "backend.h"

const hal_gpu_backend_t *hal_backend_gpu(void)
{
    return hal_is_android() ? hal_backend_gpu_android()
                            : hal_backend_gpu_linux();
}

const hal_bt_backend_t *hal_backend_bluetooth(void)
{
    return hal_is_android() ? hal_backend_bluetooth_android()
                            : hal_backend_bluetooth_linux();
}

const hal_audio_backend_t *hal_backend_audio(void)
{
    return hal_is_android() ? hal_backend_audio_android()
                            : hal_backend_audio_linux();
}
