/****************************************************************************
 * Gemini-S1 asynchronous device settings service
 ****************************************************************************/

#include <nuttx/config.h>

#include "device_settings.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/network_manager.h"
#include "infra/wifi_portal.h"
#include "infra/wifi_radio_guard.h"
#include "tools/tool_get_time.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netutils/netlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wireless/wapi.h>

#ifdef CONFIG_IEEE80211_REALTEK_WIFI
#include <arch/chip/realtek_wlan.h>

/* realtek_wlan.h deliberately exposes a board-neutral int mode argument.
 * RTW_MODE_STA is 1 in the board driver's public mode ABI; keep its private
 * wifi_constants.h out of the reusable ai_agent include path.
 */
#define DEVICE_SETTINGS_REALTEK_STA_MODE 1
#endif

#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
#include <advertiser_data.h>
#include <bluetooth.h>
#include <bt_adapter.h>
#include <bt_addr.h>
#include <bt_device.h>
#include <bt_le_scan.h>
#endif

#define TAG "dev_settings"
#define SETTINGS_QUEUE_LEN 8
#define SETTINGS_LISTENER_MAX 4
#define WIFI_SCAN_POLLS 25
#define WIFI_PORTAL_DEFAULT_TIMEOUT_SEC 300U
#define WIFI_PORTAL_MAX_TIMEOUT_SEC 600U
#define WIFI_PORTAL_HTTP_HANDOFF_GRACE_US 1500000
#define WIFI_CONFIG_SETTLE_US 250000
#define WIFI_LINK_MONITOR_SECONDS 5
#define WIFI_FACTORY_CONNECT_ATTEMPTS 3
#define WIFI_FACTORY_RETRY_DELAY_US 2000000

typedef enum {
    CMD_NONE = 0,
    CMD_WIFI_START,
    CMD_WIFI_SCAN,
    CMD_WIFI_CONNECT,
    CMD_WIFI_FACTORY_CONNECT,
    CMD_WIFI_DISCONNECT,
    CMD_WIFI_FORGET,
    CMD_WIFI_PORTAL_OPEN,
    CMD_WIFI_PORTAL_CLOSE,
    CMD_BLE_ENABLE,
    CMD_BLE_DISABLE,
    CMD_BLE_SCAN,
    CMD_BLE_STOP_SCAN,
    CMD_BLE_PAIR,
    CMD_BLE_UNPAIR,
    CMD_STOP,
} settings_cmd_type_t;

typedef struct {
    settings_cmd_type_t type;
    char arg1[65];
    char arg2[65];
} settings_cmd_t;

typedef struct {
    device_settings_listener_t cb;
    void *arg;
} settings_listener_t;

static struct {
    bool initialized;
    bool running;
    bool wifi_cancel;
    bool wifi_bootstrap_pending;
    bool time_sync_inflight;
    bool time_synced;
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    settings_cmd_t queue[SETTINGS_QUEUE_LEN];
    unsigned int qhead;
    unsigned int qtail;
    unsigned int qcount;
    settings_listener_t listeners[SETTINGS_LISTENER_MAX];
    device_wifi_status_t wifi;
    device_wifi_portal_status_t portal;
    struct timespec portal_deadline;
    device_ble_status_t ble;
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
    bt_instance_t *bt;
    bt_scanner_t *scanner;
    void *adapter_callback;
#endif
} g_settings = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

#ifdef CONFIG_AI_AGENT_DEFAULT_WIFI_SSID
#define FACTORY_WIFI_PRIMARY_SSID CONFIG_AI_AGENT_DEFAULT_WIFI_SSID
#else
#define FACTORY_WIFI_PRIMARY_SSID ""
#endif

#ifdef CONFIG_AI_AGENT_DEFAULT_WIFI_PASSWORD
#define FACTORY_WIFI_PRIMARY_PASSWORD CONFIG_AI_AGENT_DEFAULT_WIFI_PASSWORD
#else
#define FACTORY_WIFI_PRIMARY_PASSWORD ""
#endif

#ifdef CONFIG_AI_AGENT_SECONDARY_WIFI_SSID
#define FACTORY_WIFI_SECONDARY_SSID CONFIG_AI_AGENT_SECONDARY_WIFI_SSID
#else
#define FACTORY_WIFI_SECONDARY_SSID ""
#endif

#ifdef CONFIG_AI_AGENT_SECONDARY_WIFI_PASSWORD
#define FACTORY_WIFI_SECONDARY_PASSWORD CONFIG_AI_AGENT_SECONDARY_WIFI_PASSWORD
#else
#define FACTORY_WIFI_SECONDARY_PASSWORD ""
#endif

static bool is_factory_wifi_ssid(const char *ssid)
{
    if (!ssid || !ssid[0])
        return false;
    return (FACTORY_WIFI_PRIMARY_SSID[0] != '\0' &&
        strcmp(ssid, FACTORY_WIFI_PRIMARY_SSID) == 0) ||
        (FACTORY_WIFI_SECONDARY_SSID[0] != '\0' &&
        strcmp(ssid, FACTORY_WIFI_SECONDARY_SSID) == 0);
}

static void notify_listeners(device_settings_event_t event)
{
    settings_listener_t copy[SETTINGS_LISTENER_MAX];
    size_t i;

    pthread_mutex_lock(&g_settings.lock);
    memcpy(copy, g_settings.listeners, sizeof(copy));
    pthread_mutex_unlock(&g_settings.lock);

    for (i = 0; i < SETTINGS_LISTENER_MAX; i++) {
        if (copy[i].cb)
            copy[i].cb(event, copy[i].arg);
    }
}

static void copy_text(char *dest, size_t dest_size, const char *src)
{
    size_t length;

    if (!dest || dest_size == 0)
        return;
    if (!src) {
        dest[0] = '\0';
        return;
    }

    length = strnlen(src, dest_size - 1);
    memcpy(dest, src, length);
    dest[length] = '\0';
}

static int enqueue(settings_cmd_type_t type, const char *arg1,
    const char *arg2)
{
    settings_cmd_t *cmd;

    pthread_mutex_lock(&g_settings.lock);
    if (!g_settings.running) {
        pthread_mutex_unlock(&g_settings.lock);
        return -ESHUTDOWN;
    }

    if (g_settings.qcount >= SETTINGS_QUEUE_LEN) {
        pthread_mutex_unlock(&g_settings.lock);
        return -EBUSY;
    }

    cmd = &g_settings.queue[g_settings.qtail];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    copy_text(cmd->arg1, sizeof(cmd->arg1), arg1);
    copy_text(cmd->arg2, sizeof(cmd->arg2), arg2);
    g_settings.qtail = (g_settings.qtail + 1) % SETTINGS_QUEUE_LEN;
    g_settings.qcount++;
    pthread_cond_signal(&g_settings.cond);
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}

static int wifi_ap_compare(const void *a, const void *b)
{
    const device_wifi_ap_t *left = a;
    const device_wifi_ap_t *right = b;
    return right->rssi - left->rssi;
}

static int do_wifi_scan(void)
{
    struct wapi_list_s list = { 0 };
    struct wapi_scan_info_s *info;
    device_wifi_ap_t results[DEVICE_WIFI_AP_MAX];
    size_t count = 0;
    int sock;
    int ret;
    int tries;

    sock = wapi_make_socket();
    if (sock < 0)
        return sock;

    ret = wapi_set_ifup(sock, "wlan0");
    if (ret == OK)
        ret = wapi_escan_init(sock, "wlan0", IW_SCAN_TYPE_ACTIVE, NULL);

    for (tries = 0; ret == OK && tries < WIFI_SCAN_POLLS; tries++) {
        pthread_mutex_lock(&g_settings.lock);
        bool cancel = g_settings.wifi_cancel;
        pthread_mutex_unlock(&g_settings.lock);
        if (cancel) {
            close(sock);
            return -ECANCELED;
        }

        ret = wapi_scan_stat(sock, "wlan0");
        if (ret <= 0)
            break;
        usleep(200000);
    }

    if (ret == OK)
        ret = wapi_scan_coll(sock, "wlan0", &list);
    close(sock);
    if (ret < 0)
        return ret;

    memset(results, 0, sizeof(results));
    for (info = list.head.scan; info && count < DEVICE_WIFI_AP_MAX;
         info = info->next) {
        size_t i;
        if (!info->has_essid || !info->essid[0])
            continue;

        for (i = 0; i < count; i++) {
            if (strcmp(results[i].ssid, info->essid) == 0)
                break;
        }

        if (i < count && results[i].rssi >= info->rssi)
            continue;
        if (i == count)
            count++;

        snprintf(results[i].ssid, sizeof(results[i].ssid), "%s", info->essid);
        snprintf(results[i].bssid, sizeof(results[i].bssid),
            "%02x:%02x:%02x:%02x:%02x:%02x",
            info->ap.ether_addr_octet[0], info->ap.ether_addr_octet[1],
            info->ap.ether_addr_octet[2], info->ap.ether_addr_octet[3],
            info->ap.ether_addr_octet[4], info->ap.ether_addr_octet[5]);
        results[i].rssi = info->has_rssi ? info->rssi : -127;
        results[i].secure = info->has_encode && info->encode != 0;
    }

    wapi_scan_coll_free(&list);
    qsort(results, count, sizeof(results[0]), wifi_ap_compare);

    pthread_mutex_lock(&g_settings.lock);
    memcpy(g_settings.wifi.aps, results, sizeof(results));
    g_settings.wifi.ap_count = count;
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}

static int do_wifi_start(void)
{
#ifdef CONFIG_IEEE80211_REALTEK_WIFI
    return realtek_wl_initialize(DEVICE_SETTINGS_REALTEK_STA_MODE);
#else
    return 0;
#endif
}

static void refresh_wifi_network_fields(void)
{
    struct in_addr addr = { 0 };
    char ip[INET_ADDRSTRLEN] = "0.0.0.0";
    char gateway[INET_ADDRSTRLEN] = "0.0.0.0";

    if (netlib_get_ipv4addr("wlan0", &addr) == 0)
        inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    memset(&addr, 0, sizeof(addr));
    if (netlib_get_dripv4addr("wlan0", &addr) == 0)
        inet_ntop(AF_INET, &addr, gateway, sizeof(gateway));

    pthread_mutex_lock(&g_settings.lock);
    snprintf(g_settings.wifi.ip, sizeof(g_settings.wifi.ip), "%s", ip);
    snprintf(g_settings.wifi.gateway, sizeof(g_settings.wifi.gateway), "%s",
        gateway);
    pthread_mutex_unlock(&g_settings.lock);
}

static int do_wifi_disconnect(bool forget)
{
    struct in_addr zero = { .s_addr = INADDR_ANY };
    int sock = wapi_make_socket();

    if (sock >= 0) {
        wpa_driver_wext_disconnect(sock, "wlan0");
        close(sock);
    }
    netlib_set_ipv4addr("wlan0", &zero);
    netlib_set_dripv4addr("wlan0", &zero);

    if (forget) {
        config_del(AGENT_CFG_KEY_WIFI_SSID);
        config_del(AGENT_CFG_KEY_WIFI_PASS);
        config_del("wifi.configured");
        config_del("wifi.last_ssid");
    }

    pthread_mutex_lock(&g_settings.lock);
    g_settings.wifi.state = DEVICE_WIFI_IDLE;
    g_settings.wifi.error = 0;
    g_settings.wifi.ssid[0] = '\0';
    snprintf(g_settings.wifi.ip, sizeof(g_settings.wifi.ip), "0.0.0.0");
    snprintf(g_settings.wifi.gateway, sizeof(g_settings.wifi.gateway),
        "0.0.0.0");
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}

static size_t scan_wifi_for_portal(wifi_portal_network_t *networks,
    size_t capacity, void *arg)
{
    size_t count;
    size_t i;
    int ret;

    (void)arg;
    if (!networks || capacity == 0)
        return 0;

    do_wifi_disconnect(false);
    ret = do_wifi_start();
    if (ret < 0)
        return 0;

    pthread_mutex_lock(&g_settings.lock);
    g_settings.wifi_cancel = false;
    pthread_mutex_unlock(&g_settings.lock);
    ret = do_wifi_scan();
    if (ret < 0) {
        syslog(LOG_WARNING, "[%s] Portal pre-scan failed: %d\n", TAG, ret);
        return 0;
    }

    pthread_mutex_lock(&g_settings.lock);
    count = g_settings.wifi.ap_count;
    if (count > capacity)
        count = capacity;
    for (i = 0; i < count; i++) {
        copy_text(networks[i].ssid, sizeof(networks[i].ssid),
            g_settings.wifi.aps[i].ssid);
        networks[i].rssi = g_settings.wifi.aps[i].rssi;
        networks[i].secure = g_settings.wifi.aps[i].secure;
    }
    pthread_mutex_unlock(&g_settings.lock);
    return count;
}

static uint32_t portal_random_u32(void)
{
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        ssize_t nread = read(fd, &value, sizeof(value));
        close(fd);
        if (nread == sizeof(value))
            return value;
    }

    /* The platform random source is expected to be present.  This fallback
     * keeps the setup flow available during early bring-up, but is only used
     * if that device cannot be opened. */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec ^
            (uint32_t)(uintptr_t)&value;
    }
}

static uint32_t portal_remaining_locked(void)
{
    struct timespec now;
    int64_t nanoseconds;

    if (!g_settings.portal.active)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    nanoseconds = ((int64_t)g_settings.portal_deadline.tv_sec - now.tv_sec) *
        1000000000LL + g_settings.portal_deadline.tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0)
        return 0;
    return (uint32_t)((nanoseconds + 999999999LL) / 1000000000LL);
}

static void set_portal_result(bool active, bool starting, int error,
    uint32_t timeout_seconds)
{
    pthread_mutex_lock(&g_settings.lock);
    g_settings.portal.active = active;
    g_settings.portal.starting = starting;
    g_settings.portal.error = error;
    g_settings.portal.remaining_seconds = active ? timeout_seconds : 0;
    if (active) {
        clock_gettime(CLOCK_MONOTONIC, &g_settings.portal_deadline);
        g_settings.portal_deadline.tv_sec += timeout_seconds;
    } else {
        memset(&g_settings.portal_deadline, 0,
            sizeof(g_settings.portal_deadline));
    }
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_WIFI_PORTAL);
}

static int do_wifi_portal_start(uint32_t timeout_seconds)
{
    uint32_t nonce = portal_random_u32();
    char ssid[DEVICE_WIFI_PORTAL_NAME_MAX + 1];
    char password[DEVICE_WIFI_PORTAL_PASSWORD_MAX + 1];
    int ret;

    if (timeout_seconds == 0)
        timeout_seconds = WIFI_PORTAL_DEFAULT_TIMEOUT_SEC;
    if (timeout_seconds > WIFI_PORTAL_MAX_TIMEOUT_SEC)
        timeout_seconds = WIFI_PORTAL_MAX_TIMEOUT_SEC;

    snprintf(ssid, sizeof(ssid), "Gemini-S1-Setup-%04lx",
        (unsigned long)(nonce & 0xffffU));
    snprintf(password, sizeof(password), "GmS1-%08lx",
        (unsigned long)portal_random_u32());

    pthread_mutex_lock(&g_settings.lock);
    memset(&g_settings.portal, 0, sizeof(g_settings.portal));
    copy_text(g_settings.portal.ssid, sizeof(g_settings.portal.ssid), ssid);
    copy_text(g_settings.portal.password, sizeof(g_settings.portal.password),
        password);
    copy_text(g_settings.portal.url, sizeof(g_settings.portal.url),
        "http://192.168.4.1");
    pthread_mutex_unlock(&g_settings.lock);
    set_portal_result(false, true, 0, 0);

    ret = wifi_portal_start(ssid, password, scan_wifi_for_portal, NULL);
    set_portal_result(ret == 0, false, ret, timeout_seconds);
    return ret;
}

static int do_wifi_portal_open(uint32_t timeout_seconds)
{
    bool active;

    pthread_mutex_lock(&g_settings.lock);
    active = g_settings.portal.active;
    pthread_mutex_unlock(&g_settings.lock);

    /* Renewing an active session must not cycle the single Realtek radio.
     * A stop/start pair here can strand the firmware inside wifi_off() while
     * clients are associated. */
    if (active && wifi_portal_is_active()) {
        if (timeout_seconds == 0)
            timeout_seconds = WIFI_PORTAL_DEFAULT_TIMEOUT_SEC;
        if (timeout_seconds > WIFI_PORTAL_MAX_TIMEOUT_SEC)
            timeout_seconds = WIFI_PORTAL_MAX_TIMEOUT_SEC;
        set_portal_result(true, false, 0, timeout_seconds);
        syslog(LOG_INFO, "[%s] WiFi portal session renewed\n", TAG);
        return 0;
    }

    return do_wifi_portal_start(timeout_seconds);
}

static void do_wifi_portal_stop(void)
{
    wifi_portal_stop();
    set_portal_result(false, false, 0, 0);
}

#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
static void parse_ble_name(const uint8_t *data, uint8_t length, char *name,
    size_t name_size)
{
    size_t offset = 0;
    name[0] = '\0';
    while (offset + 2 <= length) {
        uint8_t field_len = data[offset];
        uint8_t type;
        size_t copy;
        if (field_len == 0 || offset + field_len >= length)
            break;
        type = data[offset + 1];
        if (type == BT_AD_NAME_COMPLETE || type == BT_AD_NAME_SHORT) {
            copy = field_len - 1;
            if (copy >= name_size)
                copy = name_size - 1;
            memcpy(name, &data[offset + 2], copy);
            name[copy] = '\0';
            return;
        }
        offset += field_len + 1;
    }
}

static void on_ble_scan_result(bt_scanner_t *scanner,
    ble_scan_result_t *result)
{
    char address[18] = { 0 };
    char name[DEVICE_BLE_NAME_MAX + 1] = { 0 };
    size_t i;
    (void)scanner;
    if (!result)
        return;

    bt_addr_ba2str(&result->addr, address);
    parse_ble_name(result->adv_data, result->length, name, sizeof(name));

    pthread_mutex_lock(&g_settings.lock);
    for (i = 0; i < g_settings.ble.device_count; i++) {
        if (strcmp(g_settings.ble.devices[i].address, address) == 0)
            break;
    }
    if (i == g_settings.ble.device_count &&
        i < DEVICE_BLE_DEVICE_MAX)
        g_settings.ble.device_count++;
    if (i < DEVICE_BLE_DEVICE_MAX) {
        device_ble_device_t *dev = &g_settings.ble.devices[i];
        snprintf(dev->address, sizeof(dev->address), "%s", address);
        snprintf(dev->name, sizeof(dev->name), "%s",
            name[0] ? name : "BLE device");
        dev->rssi = result->rssi;
        dev->bonded = bt_device_is_bonded(g_settings.bt, &result->addr,
            BT_TRANSPORT_BLE);
    }
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_BLE_SCAN);
}

static void on_ble_scan_status(bt_scanner_t *scanner, uint8_t status)
{
    (void)scanner;
    pthread_mutex_lock(&g_settings.lock);
    if (status != BT_SCAN_STATUS_SUCCESS) {
        g_settings.ble.state = DEVICE_BLE_ERROR;
        g_settings.ble.error = -EIO;
    }
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_BLE);
}

static void on_ble_scan_stopped(bt_scanner_t *scanner)
{
    (void)scanner;
    pthread_mutex_lock(&g_settings.lock);
    g_settings.scanner = NULL;
    if (g_settings.ble.state == DEVICE_BLE_SCANNING)
        g_settings.ble.state = DEVICE_BLE_READY;
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_BLE_SCAN);
}

static const scanner_callbacks_t g_scanner_callbacks = {
    .size = sizeof(scanner_callbacks_t),
    .on_scan_result = on_ble_scan_result,
    .on_scan_start_status = on_ble_scan_status,
    .on_scan_stopped = on_ble_scan_stopped,
};

static void on_bond_state(void *cookie, bt_address_t *addr,
    bt_transport_t transport, bond_state_t state, bool is_ctkd)
{
    char address[18] = { 0 };
    size_t i;
    (void)cookie;
    (void)transport;
    (void)is_ctkd;
    if (!addr)
        return;
    bt_addr_ba2str(addr, address);

    pthread_mutex_lock(&g_settings.lock);
    for (i = 0; i < g_settings.ble.device_count; i++) {
        if (strcmp(g_settings.ble.devices[i].address, address) == 0) {
            g_settings.ble.devices[i].bonded = state == BOND_STATE_BONDED;
            break;
        }
    }
    g_settings.ble.state = state == BOND_STATE_BONDING ?
        DEVICE_BLE_PAIRING : DEVICE_BLE_READY;
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_BLE);
}

static const adapter_callbacks_t g_adapter_callbacks = {
    .on_bond_state_changed = on_bond_state,
};

static int init_bluetooth(void)
{
    if (g_settings.bt)
        return 0;
    g_settings.bt = bluetooth_get_instance();
    if (!g_settings.bt)
        return -ENODEV;
    g_settings.adapter_callback = bt_adapter_register_callback(g_settings.bt,
        &g_adapter_callbacks);
    return g_settings.adapter_callback ? 0 : -EIO;
}

static int do_ble_scan(void)
{
    int ret = init_bluetooth();
    if (ret < 0)
        return ret;
    if (g_settings.scanner)
        return -EBUSY;
    memset(g_settings.ble.devices, 0, sizeof(g_settings.ble.devices));
    g_settings.ble.device_count = 0;
    g_settings.scanner = bt_le_start_scan(g_settings.bt,
        &g_scanner_callbacks);
    return g_settings.scanner ? 0 : -EIO;
}

static int do_ble_pair(const char *address, bool unpair)
{
    bt_address_t addr;
    bt_status_t status;
    int ret = init_bluetooth();
    if (ret < 0)
        return ret;
    if (bt_addr_str2ba(address, &addr) != 0)
        return -EINVAL;
    status = unpair ? bt_device_remove_bond(g_settings.bt, &addr,
        BT_TRANSPORT_BLE) : bt_device_create_bond(g_settings.bt, &addr,
        BT_TRANSPORT_BLE);
    return status == BT_STATUS_SUCCESS ? 0 : -EIO;
}
#endif

static void set_wifi_result(device_wifi_state_t state, int error)
{
    pthread_mutex_lock(&g_settings.lock);
    g_settings.wifi.state = state;
    g_settings.wifi.error = error;
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_WIFI);
}

/* The default or previously configured network is selected asynchronously
 * during boot.  Keep that one serialized transaction visible to the network
 * watcher so it cannot reset the Realtek radio halfway through a scan. */
static void finish_wifi_bootstrap(void)
{
    bool finished = false;

    pthread_mutex_lock(&g_settings.lock);
    if (g_settings.wifi_bootstrap_pending) {
        g_settings.wifi_bootstrap_pending = false;
        finished = true;
    }
    pthread_mutex_unlock(&g_settings.lock);

    if (finished)
        syslog(LOG_INFO, "[%s] Boot WiFi selection completed\n", TAG);
}

static void refresh_wifi_runtime_state(void)
{
    device_wifi_state_t old_state;
    device_wifi_state_t new_state;
    bool ready;

    /* Keep the blocking WAPI association query on this worker, never on the
     * LVGL snapshot path.  If provisioning or recovery owns the radio, the
     * next periodic pass will observe the settled result. */
    if (wifi_radio_guard_try_acquire() < 0)
        return;
    ready = network_wifi_link_is_ready();
    wifi_radio_guard_release();
    refresh_wifi_network_fields();

    pthread_mutex_lock(&g_settings.lock);
    old_state = g_settings.wifi.state;
    new_state = old_state;
    if (old_state != DEVICE_WIFI_SCANNING &&
        old_state != DEVICE_WIFI_ASSOCIATING) {
        if (ready)
            new_state = DEVICE_WIFI_ONLINE;
        else if (old_state == DEVICE_WIFI_ONLINE)
            new_state = DEVICE_WIFI_IDLE;
    }
    if (new_state != old_state) {
        g_settings.wifi.state = new_state;
        g_settings.wifi.error = 0;
    }
    pthread_mutex_unlock(&g_settings.lock);

    if (new_state != old_state)
        notify_listeners(DEVICE_SETTINGS_EVENT_WIFI);
}

/* Request validated NTP time only after the Wi-Fi path is usable.  This
 * worker is detached so a slow DNS/UDP request never blocks Wi-Fi, BLE, or
 * LVGL settings events.  tool_get_time_execute stores UTC in CLOCK_REALTIME
 * and reports its result in Beijing time (UTC+8). */
static void *beijing_time_sync_worker(void *arg)
{
    char result[96];
    int ret;
    (void)arg;

    ret = tool_get_time_execute(NULL, result, sizeof(result));
    pthread_mutex_lock(&g_settings.lock);
    g_settings.time_sync_inflight = false;
    g_settings.time_synced = ret == 0;
    pthread_mutex_unlock(&g_settings.lock);
    syslog(ret == 0 ? LOG_INFO : LOG_WARNING,
           "[%s] Beijing time sync %s: %s\n", TAG,
           ret == 0 ? "completed" : "failed", result);
    return NULL;
}

static void start_beijing_time_sync(void)
{
    pthread_attr_t attr;
    pthread_t worker;
    int ret;

    pthread_mutex_lock(&g_settings.lock);
    if (g_settings.time_synced || g_settings.time_sync_inflight) {
        pthread_mutex_unlock(&g_settings.lock);
        return;
    }
    g_settings.time_sync_inflight = true;
    pthread_mutex_unlock(&g_settings.lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&worker, &attr, beijing_time_sync_worker, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        pthread_mutex_lock(&g_settings.lock);
        g_settings.time_sync_inflight = false;
        pthread_mutex_unlock(&g_settings.lock);
        syslog(LOG_WARNING, "[%s] Cannot start Beijing time sync: %d\n",
               TAG, ret);
    }
}

static bool wifi_scan_has_ssid(const char *ssid)
{
    size_t i;
    bool found = false;

    if (!ssid || !ssid[0])
        return false;

    pthread_mutex_lock(&g_settings.lock);
    for (i = 0; i < g_settings.wifi.ap_count; i++) {
        if (strcmp(g_settings.wifi.aps[i].ssid, ssid) == 0) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_settings.lock);
    return found;
}

static int do_wifi_connect_credentials(const char *ssid,
    const char *password)
{
    bool portal_handoff = wifi_portal_is_active();
    int ret;

    /* Keep the concurrent wlan1 AP alive while wlan0 joins the new network.
     * The HTTP response gets a short head start, and the shared radio guard
     * excludes the background reconnect watcher. */
    if (portal_handoff)
        usleep(WIFI_PORTAL_HTTP_HANDOFF_GRACE_US);
    ret = wifi_portal_begin_sta_handoff();
    if (ret < 0) {
        set_wifi_result(DEVICE_WIFI_ERROR, ret);
        return ret;
    }
    if (!portal_handoff) {
        ret = do_wifi_start();
        if (ret < 0) {
            wifi_portal_end_sta_handoff();
            set_portal_result(false, false, ret, 0);
            set_wifi_result(DEVICE_WIFI_ERROR, ret);
            return ret;
        }
    }

    set_wifi_result(DEVICE_WIFI_ASSOCIATING, 0);
    ret = network_wifi_connect("wlan0", ssid, password);
    if (ret == 0) {
        if (agent_config_set("wifi.configured", "1") == OK)
            usleep(WIFI_CONFIG_SETTLE_US);
        if (agent_config_set("wifi.last_ssid", ssid) == OK)
            usleep(WIFI_CONFIG_SETTLE_US);
        pthread_mutex_lock(&g_settings.lock);
        copy_text(g_settings.wifi.ssid, sizeof(g_settings.wifi.ssid), ssid);
        pthread_mutex_unlock(&g_settings.lock);
        refresh_wifi_network_fields();
        set_wifi_result(DEVICE_WIFI_ONLINE, 0);
        start_beijing_time_sync();
    } else {
        set_wifi_result(DEVICE_WIFI_ERROR, ret);
    }
    wifi_portal_end_sta_handoff();
    set_portal_result(false, false, 0, 0);
    return ret;
}

static int do_factory_wifi_connect_once(void)
{
    bool primary_visible = false;
    bool secondary_visible = false;
    bool scan_ok;
    int ret;

    do_wifi_portal_stop();
    ret = do_wifi_start();
    if (ret < 0) {
        set_wifi_result(DEVICE_WIFI_ERROR, ret);
        return ret;
    }

    pthread_mutex_lock(&g_settings.lock);
    g_settings.wifi_cancel = false;
    pthread_mutex_unlock(&g_settings.lock);
    set_wifi_result(DEVICE_WIFI_SCANNING, 0);
    ret = do_wifi_scan();
    scan_ok = ret == 0;
    if (scan_ok) {
        primary_visible = wifi_scan_has_ssid(FACTORY_WIFI_PRIMARY_SSID);
        secondary_visible = wifi_scan_has_ssid(FACTORY_WIFI_SECONDARY_SSID);
    } else {
        syslog(LOG_WARNING,
            "[%s] Factory WiFi scan failed: %d; trying configured profiles\n",
            TAG, ret);
        primary_visible = FACTORY_WIFI_PRIMARY_SSID[0] != '\0';
        secondary_visible = FACTORY_WIFI_SECONDARY_SSID[0] != '\0';
    }
    notify_listeners(DEVICE_SETTINGS_EVENT_WIFI_SCAN);

    /* A scan is only an optimization.  Some APs suppress beacons, and the
     * Realtek driver can return an incomplete scan during its first seconds
     * after power-on.  The private build still has explicit credentials, so
     * attempt them even when the SSID is not listed. */
    if (!primary_visible && !secondary_visible && scan_ok) {
        syslog(LOG_INFO,
            "[%s] Factory WiFi not listed by scan; trying configured profiles\n",
            TAG);
    }

    if (FACTORY_WIFI_PRIMARY_SSID[0] != '\0') {
        syslog(LOG_INFO, "[%s] Trying %s primary factory WiFi\n", TAG,
            primary_visible ? "visible" : "configured");
        ret = do_wifi_connect_credentials(FACTORY_WIFI_PRIMARY_SSID,
            FACTORY_WIFI_PRIMARY_PASSWORD);
        if (ret == 0)
            return 0;
    }
    if (FACTORY_WIFI_SECONDARY_SSID[0] != '\0') {
        syslog(LOG_INFO, "[%s] Trying %s secondary factory WiFi\n", TAG,
            secondary_visible ? "visible" : "configured");
        ret = do_wifi_connect_credentials(FACTORY_WIFI_SECONDARY_SSID,
            FACTORY_WIFI_SECONDARY_PASSWORD);
        if (ret == 0)
            return 0;
    }

    set_wifi_result(DEVICE_WIFI_ERROR, ret);
    return ret;
}

static int do_factory_wifi_connect(void)
{
    int ret = -ENETUNREACH;
    unsigned int attempt;

    /* The first association can time out while the RTL8723F firmware is
     * settling after cold power-on.  Retry before exposing the setup AP:
     * once that AP owns the shared radio, the ordinary network watcher
     * cannot retry until its provisioning window expires. */
    for (attempt = 0; attempt < WIFI_FACTORY_CONNECT_ATTEMPTS; attempt++) {
        bool running;

        pthread_mutex_lock(&g_settings.lock);
        running = g_settings.running;
        pthread_mutex_unlock(&g_settings.lock);
        if (!running)
            return -ESHUTDOWN;

        ret = do_factory_wifi_connect_once();
        if (ret == 0)
            return 0;
        if (attempt + 1 < WIFI_FACTORY_CONNECT_ATTEMPTS) {
            syslog(LOG_WARNING, "[%s] Factory WiFi attempt %u/%u failed; "
                "retrying\n", TAG, attempt + 1,
                WIFI_FACTORY_CONNECT_ATTEMPTS);
            usleep(WIFI_FACTORY_RETRY_DELAY_US);
        }
    }

    return ret;
}

static void set_ble_result(device_ble_state_t state, int error)
{
    pthread_mutex_lock(&g_settings.lock);
    g_settings.ble.state = state;
    g_settings.ble.error = error;
    pthread_mutex_unlock(&g_settings.lock);
    notify_listeners(DEVICE_SETTINGS_EVENT_BLE);
}

static void *settings_worker(void *arg)
{
    settings_cmd_t cmd;
    int ret;
    (void)arg;

    for (;;) {
        int wait_ret = 0;

        pthread_mutex_lock(&g_settings.lock);
        while (g_settings.qcount == 0 && g_settings.running) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += WIFI_LINK_MONITOR_SECONDS;
            wait_ret = pthread_cond_timedwait(&g_settings.cond,
                &g_settings.lock, &deadline);
            if (wait_ret == ETIMEDOUT)
                break;
        }
        if (!g_settings.running && g_settings.qcount == 0) {
            pthread_mutex_unlock(&g_settings.lock);
            break;
        }
        if (g_settings.qcount == 0) {
            pthread_mutex_unlock(&g_settings.lock);
            refresh_wifi_runtime_state();
            continue;
        }
        cmd = g_settings.queue[g_settings.qhead];
        g_settings.qhead = (g_settings.qhead + 1) % SETTINGS_QUEUE_LEN;
        g_settings.qcount--;
        pthread_mutex_unlock(&g_settings.lock);

        if (cmd.type == CMD_STOP)
            break;
        switch (cmd.type) {
        case CMD_WIFI_START:
            do_wifi_portal_stop();
            ret = do_wifi_start();
            if (ret < 0)
                set_wifi_result(DEVICE_WIFI_ERROR, ret);
            else
                set_wifi_result(DEVICE_WIFI_IDLE, 0);
            break;
        case CMD_WIFI_SCAN:
            do_wifi_portal_stop();
            ret = do_wifi_start();
            if (ret < 0) {
                set_wifi_result(DEVICE_WIFI_ERROR, ret);
                notify_listeners(DEVICE_SETTINGS_EVENT_WIFI_SCAN);
                break;
            }
            pthread_mutex_lock(&g_settings.lock);
            g_settings.wifi_cancel = false;
            pthread_mutex_unlock(&g_settings.lock);
            set_wifi_result(DEVICE_WIFI_SCANNING, 0);
            ret = do_wifi_scan();
            set_wifi_result(ret == 0 ? DEVICE_WIFI_IDLE : DEVICE_WIFI_ERROR,
                ret);
            notify_listeners(DEVICE_SETTINGS_EVENT_WIFI_SCAN);
            break;
        case CMD_WIFI_CONNECT:
            do_wifi_connect_credentials(cmd.arg1, cmd.arg2);
            finish_wifi_bootstrap();
            break;
        case CMD_WIFI_FACTORY_CONNECT:
            do_factory_wifi_connect();
            finish_wifi_bootstrap();
            break;
        case CMD_WIFI_DISCONNECT:
            do_wifi_disconnect(false);
            notify_listeners(DEVICE_SETTINGS_EVENT_WIFI);
            break;
        case CMD_WIFI_FORGET:
            do_wifi_disconnect(true);
            notify_listeners(DEVICE_SETTINGS_EVENT_WIFI);
            break;
        case CMD_WIFI_PORTAL_OPEN:
            ret = do_wifi_portal_open((uint32_t)strtoul(cmd.arg1, NULL, 10));
            if (ret < 0)
                set_wifi_result(DEVICE_WIFI_ERROR, ret);
            else
                set_wifi_result(DEVICE_WIFI_IDLE, 0);
            break;
        case CMD_WIFI_PORTAL_CLOSE:
            do_wifi_portal_stop();
            set_wifi_result(DEVICE_WIFI_IDLE, 0);
            break;
        case CMD_BLE_ENABLE:
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
            ret = init_bluetooth();
            if (ret == 0 && bt_adapter_get_state(g_settings.bt) <
                BT_ADAPTER_STATE_BLE_ON)
                ret = bt_adapter_enable_le(g_settings.bt) ==
                    BT_STATUS_SUCCESS ? 0 : -EIO;
            set_ble_result(ret == 0 ? DEVICE_BLE_READY : DEVICE_BLE_ERROR,
                ret);
#else
            set_ble_result(DEVICE_BLE_ERROR, -ENOTSUP);
#endif
            break;
        case CMD_BLE_DISABLE:
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
            if (g_settings.scanner) {
                bt_le_stop_scan(g_settings.bt, g_settings.scanner);
                g_settings.scanner = NULL;
            }
            ret = g_settings.bt && bt_adapter_disable_le(g_settings.bt) !=
                BT_STATUS_SUCCESS ? -EIO : 0;
            set_ble_result(ret == 0 ? DEVICE_BLE_OFF : DEVICE_BLE_ERROR, ret);
#else
            set_ble_result(DEVICE_BLE_OFF, 0);
#endif
            break;
        case CMD_BLE_SCAN:
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
            set_ble_result(DEVICE_BLE_SCANNING, 0);
            ret = do_ble_scan();
            if (ret < 0)
                set_ble_result(DEVICE_BLE_ERROR, ret);
#else
            set_ble_result(DEVICE_BLE_ERROR, -ENOTSUP);
#endif
            break;
        case CMD_BLE_STOP_SCAN:
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
            if (g_settings.scanner) {
                bt_le_stop_scan(g_settings.bt, g_settings.scanner);
                g_settings.scanner = NULL;
            }
#endif
            set_ble_result(DEVICE_BLE_READY, 0);
            break;
        case CMD_BLE_PAIR:
        case CMD_BLE_UNPAIR:
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
            set_ble_result(DEVICE_BLE_PAIRING, 0);
            ret = do_ble_pair(cmd.arg1, cmd.type == CMD_BLE_UNPAIR);
            if (ret < 0)
                set_ble_result(DEVICE_BLE_ERROR, ret);
#else
            set_ble_result(DEVICE_BLE_ERROR, -ENOTSUP);
#endif
            break;
        default:
            break;
        }
    }
    return NULL;
}

int device_settings_init(void)
{
    pthread_attr_t attr;
    char ssid[DEVICE_SSID_MAX + 1] = { 0 };
    char pass[65] = { 0 };
    bool has_factory_wifi;
    bool has_saved_wifi;
    int ret;

    pthread_mutex_lock(&g_settings.lock);
    if (g_settings.initialized) {
        pthread_mutex_unlock(&g_settings.lock);
        return 0;
    }
    memset(&g_settings.wifi, 0, sizeof(g_settings.wifi));
    memset(&g_settings.ble, 0, sizeof(g_settings.ble));
    memset(&g_settings.portal, 0, sizeof(g_settings.portal));
    copy_text(g_settings.portal.url, sizeof(g_settings.portal.url),
        "http://192.168.4.1");
    g_settings.wifi.state = DEVICE_WIFI_IDLE;
    g_settings.ble.state = DEVICE_BLE_OFF;
    g_settings.running = true;
    g_settings.initialized = true;
    pthread_mutex_unlock(&g_settings.lock);

    config_store_init();
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    ret = pthread_create(&g_settings.worker, &attr, settings_worker, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        pthread_mutex_lock(&g_settings.lock);
        g_settings.running = false;
        g_settings.initialized = false;
        pthread_mutex_unlock(&g_settings.lock);
        return -ret;
    }

    enqueue(CMD_WIFI_START, NULL, NULL);

    has_saved_wifi =
        agent_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid)) == OK &&
        ssid[0] != '\0';
    has_factory_wifi = FACTORY_WIFI_PRIMARY_SSID[0] != '\0' ||
        FACTORY_WIFI_SECONDARY_SSID[0] != '\0';

    /* A user-provisioned network remains authoritative.  If the persisted
     * network is one of the factory profiles, scan both factory choices on
     * every boot so a previously selected AP going offline does not pin the
     * device to it forever. */
    if (has_saved_wifi &&
        (!has_factory_wifi || !is_factory_wifi_ssid(ssid))) {
        pthread_mutex_lock(&g_settings.lock);
        g_settings.wifi_bootstrap_pending = true;
        pthread_mutex_unlock(&g_settings.lock);
        agent_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, sizeof(pass));
        device_wifi_connect(ssid, pass);
    } else if (has_factory_wifi) {
        syslog(LOG_INFO,
            "[settings] Selecting an available factory WiFi profile\n");
        pthread_mutex_lock(&g_settings.lock);
        g_settings.wifi_bootstrap_pending = true;
        pthread_mutex_unlock(&g_settings.lock);
        enqueue(CMD_WIFI_FACTORY_CONNECT, NULL, NULL);
    }
    return 0;
}

void device_settings_deinit(void)
{
    pthread_mutex_lock(&g_settings.lock);
    if (!g_settings.initialized) {
        pthread_mutex_unlock(&g_settings.lock);
        return;
    }
    g_settings.running = false;
    pthread_cond_signal(&g_settings.cond);
    pthread_mutex_unlock(&g_settings.lock);
    pthread_join(g_settings.worker, NULL);
    wifi_portal_stop();
#ifdef CONFIG_AI_AGENT_BLE_DEVICE_SETTINGS
    if (g_settings.adapter_callback && g_settings.bt)
        bt_adapter_unregister_callback(g_settings.bt,
            g_settings.adapter_callback);
#endif
    pthread_mutex_lock(&g_settings.lock);
    g_settings.initialized = false;
    pthread_mutex_unlock(&g_settings.lock);
}

bool device_wifi_bootstrap_in_progress(void)
{
    bool pending;

    pthread_mutex_lock(&g_settings.lock);
    pending = g_settings.initialized && g_settings.wifi_bootstrap_pending;
    pthread_mutex_unlock(&g_settings.lock);
    return pending;
}

int device_settings_register_listener(device_settings_listener_t cb,
    void *arg)
{
    size_t i;
    if (!cb)
        return -EINVAL;
    pthread_mutex_lock(&g_settings.lock);
    for (i = 0; i < SETTINGS_LISTENER_MAX; i++) {
        if (!g_settings.listeners[i].cb) {
            g_settings.listeners[i].cb = cb;
            g_settings.listeners[i].arg = arg;
            pthread_mutex_unlock(&g_settings.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_settings.lock);
    return -ENOSPC;
}

int device_settings_unregister_listener(device_settings_listener_t cb,
    void *arg)
{
    size_t i;
    if (!cb)
        return -EINVAL;
    pthread_mutex_lock(&g_settings.lock);
    for (i = 0; i < SETTINGS_LISTENER_MAX; i++) {
        if (g_settings.listeners[i].cb == cb &&
            g_settings.listeners[i].arg == arg) {
            memset(&g_settings.listeners[i], 0,
                   sizeof(g_settings.listeners[i]));
            pthread_mutex_unlock(&g_settings.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_settings.lock);
    return -ENOENT;
}

bool device_wifi_is_configured(void)
{
    char ssid[DEVICE_SSID_MAX + 1] = { 0 };
    return agent_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid)) == OK &&
        ssid[0] != '\0';
}

int device_wifi_scan(void)
{
    return enqueue(CMD_WIFI_SCAN, NULL, NULL);
}

int device_wifi_cancel_scan(void)
{
    pthread_mutex_lock(&g_settings.lock);
    g_settings.wifi_cancel = true;
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}

int device_wifi_connect(const char *ssid, const char *password)
{
    size_t ssid_len;
    size_t pass_len;
    if (!ssid)
        return -EINVAL;
    ssid_len = strnlen(ssid, DEVICE_SSID_MAX + 1);
    pass_len = password ? strnlen(password, 64) : 0;
    if (ssid_len == 0 || ssid_len > DEVICE_SSID_MAX || pass_len > 63 ||
        (pass_len > 0 && pass_len < 8))
        return -EINVAL;
    return enqueue(CMD_WIFI_CONNECT, ssid, password ? password : "");
}

int device_wifi_disconnect(void)
{
    return enqueue(CMD_WIFI_DISCONNECT, NULL, NULL);
}

int device_wifi_forget(void)
{
    return enqueue(CMD_WIFI_FORGET, NULL, NULL);
}

int device_wifi_portal_open(uint32_t timeout_seconds)
{
    char timeout[12];
    int ret;
    bool active;
    bool starting;

    if (timeout_seconds == 0)
        timeout_seconds = WIFI_PORTAL_DEFAULT_TIMEOUT_SEC;
    if (timeout_seconds > WIFI_PORTAL_MAX_TIMEOUT_SEC)
        timeout_seconds = WIFI_PORTAL_MAX_TIMEOUT_SEC;
    snprintf(timeout, sizeof(timeout), "%lu", (unsigned long)timeout_seconds);

    pthread_mutex_lock(&g_settings.lock);
    active = g_settings.portal.active;
    starting = g_settings.portal.starting;
    pthread_mutex_unlock(&g_settings.lock);
    if (starting)
        return 0;

    ret = enqueue(CMD_WIFI_PORTAL_OPEN, timeout, NULL);
    if (ret == 0 && !active) {
        pthread_mutex_lock(&g_settings.lock);
        g_settings.portal.active = false;
        g_settings.portal.starting = true;
        g_settings.portal.error = 0;
        pthread_mutex_unlock(&g_settings.lock);
        notify_listeners(DEVICE_SETTINGS_EVENT_WIFI_PORTAL);
    }
    return ret;
}

int device_wifi_portal_close(void)
{
    return enqueue(CMD_WIFI_PORTAL_CLOSE, NULL, NULL);
}

int device_wifi_portal_get_status(device_wifi_portal_status_t *status)
{
    bool expired = false;

    if (!status)
        return -EINVAL;
    pthread_mutex_lock(&g_settings.lock);
    g_settings.portal.remaining_seconds = portal_remaining_locked();
    *status = g_settings.portal;
    expired = status->active && status->remaining_seconds == 0;
    pthread_mutex_unlock(&g_settings.lock);

    /* The UI only reads a snapshot.  Queue the destructive transition for
     * the serialized worker so a slow Wi-Fi driver never runs on LVGL's
     * timer thread. */
    if (expired)
        device_wifi_portal_close();
    return 0;
}

int device_wifi_get_status(device_wifi_status_t *status)
{
    if (!status)
        return -EINVAL;

    /* Keep status reads non-blocking.  The launcher calls this function from
     * the LVGL timer thread once per second, so synchronous WAPI ioctls here
     * can stop all screen rendering while the radio is down or associating.
     * The serialized settings worker owns ESSID updates; only the lightweight
     * netlib address cache is refreshed on demand. */
    refresh_wifi_network_fields();
    pthread_mutex_lock(&g_settings.lock);
    *status = g_settings.wifi;
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}

int device_ble_set_enabled(bool enabled)
{
    return enqueue(enabled ? CMD_BLE_ENABLE : CMD_BLE_DISABLE, NULL, NULL);
}

int device_ble_scan(void)
{
    return enqueue(CMD_BLE_SCAN, NULL, NULL);
}

int device_ble_cancel_scan(void)
{
    return enqueue(CMD_BLE_STOP_SCAN, NULL, NULL);
}

int device_ble_pair(const char *address)
{
    return address ? enqueue(CMD_BLE_PAIR, address, NULL) : -EINVAL;
}

int device_ble_unpair(const char *address)
{
    return address ? enqueue(CMD_BLE_UNPAIR, address, NULL) : -EINVAL;
}

int device_ble_get_status(device_ble_status_t *status)
{
    if (!status)
        return -EINVAL;
    pthread_mutex_lock(&g_settings.lock);
    *status = g_settings.ble;
    pthread_mutex_unlock(&g_settings.lock);
    return 0;
}
