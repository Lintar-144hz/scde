#ifndef HAL_BLUETOOTH_H
#define HAL_BLUETOOTH_H

#include <hal/hal.h>

typedef struct {
    uint32_t index;
    char name[64];
    char address[18];
    bool powered;
    bool power_known;
    bool blocked_by_rfkill;
} hal_bt_adapter_t;

#define HAL_BT_CAP_ADAPTER (1u << 0)
#define HAL_BT_CAP_POWER (1u << 1)
#define HAL_BT_CAP_DISCOVERY (1u << 2)

uint32_t hal_bluetooth_capabilities(void);
bool hal_bluetooth_available(void);
int hal_bluetooth_adapter_count(void);
hal_status_t hal_bluetooth_get_adapter(int index, hal_bt_adapter_t *out);
hal_status_t hal_bluetooth_set_powered(bool on);
hal_status_t hal_bluetooth_get_powered(bool *on);
hal_status_t hal_bluetooth_set_discovery(bool on);

#endif
