#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_cmd_handler_recv(const uint8_t *data, uint16_t len, void *user_data);
int ble_provisioning_open(uint32_t timeout_sec);
void ble_provisioning_close(void);
bool ble_provisioning_is_open(void);
uint32_t ble_provisioning_remaining(void);
int ble_provisioning_get_pin(char pin[7]);

#ifdef __cplusplus
}
#endif
