/****************************************************************************
 * Gemini-S1 device settings service
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_WIFI_AP_MAX 20
#define DEVICE_BLE_DEVICE_MAX 30
#define DEVICE_SSID_MAX 32
#define DEVICE_BLE_NAME_MAX 30
#define DEVICE_WIFI_PORTAL_NAME_MAX 32
#define DEVICE_WIFI_PORTAL_PASSWORD_MAX 63

typedef enum {
    DEVICE_WIFI_IDLE = 0,
    DEVICE_WIFI_SCANNING,
    DEVICE_WIFI_ASSOCIATING,
    DEVICE_WIFI_DHCP,
    DEVICE_WIFI_ONLINE,
    DEVICE_WIFI_ERROR,
} device_wifi_state_t;

typedef enum {
    DEVICE_BLE_OFF = 0,
    DEVICE_BLE_IDLE,
    DEVICE_BLE_SCANNING,
    DEVICE_BLE_PAIRING,
    DEVICE_BLE_READY,
    DEVICE_BLE_ERROR,
} device_ble_state_t;

typedef enum {
    DEVICE_SETTINGS_EVENT_WIFI = 1,
    DEVICE_SETTINGS_EVENT_WIFI_SCAN,
    DEVICE_SETTINGS_EVENT_BLE,
    DEVICE_SETTINGS_EVENT_BLE_SCAN,
    DEVICE_SETTINGS_EVENT_PROVISIONING,
    DEVICE_SETTINGS_EVENT_WIFI_PORTAL,
} device_settings_event_t;

typedef struct {
    char ssid[DEVICE_SSID_MAX + 1];
    char bssid[18];
    int rssi;
    bool secure;
} device_wifi_ap_t;

typedef struct device_wifi_status {
    device_wifi_state_t state;
    char ssid[DEVICE_SSID_MAX + 1];
    char ip[16];
    char gateway[16];
    int rssi;
    int error;
    size_t ap_count;
    device_wifi_ap_t aps[DEVICE_WIFI_AP_MAX];
} device_wifi_status_t;

typedef struct {
    char name[DEVICE_BLE_NAME_MAX + 1];
    char address[18];
    int rssi;
    bool bonded;
} device_ble_device_t;

typedef struct {
    device_ble_state_t state;
    int error;
    size_t device_count;
    device_ble_device_t devices[DEVICE_BLE_DEVICE_MAX];
} device_ble_status_t;

typedef struct {
    bool active;
    bool starting;
    uint32_t remaining_seconds;
    int error;
    char ssid[DEVICE_WIFI_PORTAL_NAME_MAX + 1];
    char password[DEVICE_WIFI_PORTAL_PASSWORD_MAX + 1];
    char url[24];
} device_wifi_portal_status_t;

typedef void (*device_settings_listener_t)(device_settings_event_t event,
    void *arg);

int device_settings_init(void);
void device_settings_deinit(void);
/* True while the boot-selected network is scanning or associating.  The
 * network watcher must not issue a competing reconnect while the single
 * Wi-Fi radio is owned by this startup transaction. */
bool device_wifi_bootstrap_in_progress(void);
int device_settings_register_listener(device_settings_listener_t cb,
    void *arg);
int device_settings_unregister_listener(device_settings_listener_t cb,
    void *arg);
bool device_wifi_is_configured(void);
int device_wifi_scan(void);
int device_wifi_cancel_scan(void);
int device_wifi_connect(const char *ssid, const char *password);
int device_wifi_disconnect(void);
int device_wifi_forget(void);
int device_wifi_get_status(device_wifi_status_t *status);
int device_wifi_portal_open(uint32_t timeout_seconds);
int device_wifi_portal_close(void);
int device_wifi_portal_get_status(device_wifi_portal_status_t *status);
int device_ble_set_enabled(bool enabled);
int device_ble_scan(void);
int device_ble_cancel_scan(void);
int device_ble_pair(const char *address);
int device_ble_unpair(const char *address);
int device_ble_get_status(device_ble_status_t *status);

#ifdef __cplusplus
}
#endif
