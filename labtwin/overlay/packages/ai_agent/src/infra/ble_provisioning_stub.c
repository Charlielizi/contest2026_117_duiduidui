/* BLE provisioning fallback for configurations without the NUS GATT server. */

#include "ble_cmd_handler.h"

#include <errno.h>

int ble_provisioning_open(uint32_t timeout_sec)
{
    (void)timeout_sec;
    return -ENOTSUP;
}

void ble_provisioning_close(void)
{
}

bool ble_provisioning_is_open(void)
{
    return false;
}

uint32_t ble_provisioning_remaining(void)
{
    return 0;
}

int ble_provisioning_get_pin(char pin[7])
{
    if (pin)
        pin[0] = '\0';
    return -ENOTSUP;
}
