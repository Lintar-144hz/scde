#include <hal/hal.h>

#include "backends/linux/linux_util.h"

bool hal_is_android(void)
{
    return hal_linux_exists("/system/build.prop") ||
           hal_linux_exists("/vendor/build.prop") ||
           hal_linux_exists("/system/bin/app_process64") ||
           hal_linux_exists("/system/bin/app_process32");
}
