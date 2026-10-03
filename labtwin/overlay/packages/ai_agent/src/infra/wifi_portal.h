/****************************************************************************
 * Gemini-S1 temporary Wi-Fi hotspot provisioning portal
 ****************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*wifi_portal_submit_cb_t)(const char *ssid,
    const char *password, void *arg);

#define WIFI_PORTAL_NETWORK_MAX 20
#define WIFI_PORTAL_SSID_MAX 32

typedef struct {
    char ssid[WIFI_PORTAL_SSID_MAX + 1];
    int rssi;
    bool secure;
} wifi_portal_network_t;

typedef size_t (*wifi_portal_scan_cb_t)(wifi_portal_network_t *networks,
    size_t capacity, void *arg);

int wifi_portal_start(const char *ssid, const char *password,
    wifi_portal_scan_cb_t scan, void *arg);
void wifi_portal_stop(void);
bool wifi_portal_is_active(void);
int wifi_portal_begin_sta_handoff(void);
void wifi_portal_end_sta_handoff(void);

/* Returns true when the request belonged to the temporary portal. */
bool wifi_portal_http_try_handle(int fd, const char *request,
    size_t request_len, wifi_portal_submit_cb_t submit, void *arg);

#ifdef __cplusplus
}
#endif
