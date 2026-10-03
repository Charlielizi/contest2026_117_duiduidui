/* Secure, framed BLE provisioning command handler. */

#include <nuttx/config.h>

#include "ble_cmd_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <cJSON.h>

#include "device_settings.h"
#include "infra/ble_gatt.h"

#define TAG "ble_prov"
#define BLE_FRAME_MAX 512
#define BLE_FRAME_TIMEOUT_SEC 30
#define BLE_PROVISION_DEFAULT_SEC 120

static struct {
    pthread_mutex_t lock;
    char frame[BLE_FRAME_MAX + 1];
    size_t frame_len;
    time_t frame_updated;
    time_t expires;
    char pin[7];
    bool pending_wifi;
    bool listener_registered;
} g_prov = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static void send_json(const char *json)
{
    size_t length;
    size_t offset = 0;
    uint16_t payload;
    if (!json || !ble_gatt_is_connected())
        return;
    length = strlen(json);
    payload = ble_gatt_get_mtu();
    payload = payload > 3 ? payload - 3 : 20;
    while (offset < length) {
        size_t chunk = length - offset;
        if (chunk > payload)
            chunk = payload;
        if (ble_gatt_send((const uint8_t *)json + offset,
                (uint16_t)chunk) < 0)
            break;
        offset += chunk;
        if (offset < length)
            usleep(20000);
    }
}

static void send_error(const char *code)
{
    char response[128];
    snprintf(response, sizeof(response),
        "{\"v\":1,\"cmd\":\"error\",\"ok\":false,\"code\":\"%s\"}\n",
        code ? code : "internal");
    send_json(response);
}

static uint32_t make_pin(void)
{
    uint32_t value = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t nread = read(fd, &value, sizeof(value));
        close(fd);
        if (nread != sizeof(value))
            value = 0;
    }
    if (value == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        value = (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec ^
            (uint32_t)getpid();
    }
    return 100000 + value % 900000;
}

static void provisioning_listener(device_settings_event_t event, void *arg)
{
    device_wifi_status_t status;
    bool pending;
    (void)arg;
    if (event != DEVICE_SETTINGS_EVENT_WIFI)
        return;
    pthread_mutex_lock(&g_prov.lock);
    pending = g_prov.pending_wifi;
    pthread_mutex_unlock(&g_prov.lock);
    if (!pending || device_wifi_get_status(&status) < 0)
        return;

    if (status.state == DEVICE_WIFI_ONLINE) {
        send_json("{\"v\":1,\"cmd\":\"wifi_config_result\",\"ok\":true,\"code\":\"connected\"}\n");
        ble_provisioning_close();
    } else if (status.state == DEVICE_WIFI_ERROR) {
        pthread_mutex_lock(&g_prov.lock);
        g_prov.pending_wifi = false;
        pthread_mutex_unlock(&g_prov.lock);
        send_error("wifi_failed");
    }
}

int ble_provisioning_open(uint32_t timeout_sec)
{
    ble_gatt_config_t config = {
        .device_name = "Gemini-S1-Setup",
        .recv_cb = ble_cmd_handler_recv,
    };
    uint32_t pin = make_pin();
    int ret;

    if (timeout_sec == 0)
        timeout_sec = BLE_PROVISION_DEFAULT_SEC;
    if (timeout_sec > 600)
        timeout_sec = 600;

    ret = device_settings_init();
    if (ret < 0)
        return ret;
    ret = ble_gatt_init(&config);
    if (ret < 0)
        return ret;

    pthread_mutex_lock(&g_prov.lock);
    snprintf(g_prov.pin, sizeof(g_prov.pin), "%06lu",
        (unsigned long)pin);
    g_prov.expires = time(NULL) + timeout_sec;
    g_prov.frame_len = 0;
    g_prov.pending_wifi = false;
    bool register_listener = !g_prov.listener_registered;
    g_prov.listener_registered = true;
    pthread_mutex_unlock(&g_prov.lock);

    if (register_listener)
        device_settings_register_listener(provisioning_listener, NULL);
    syslog(LOG_INFO, "[%s] provisioning window opened for %lu seconds\n",
        TAG, (unsigned long)timeout_sec);
    return 0;
}

void ble_provisioning_close(void)
{
    pthread_mutex_lock(&g_prov.lock);
    g_prov.expires = 0;
    g_prov.pin[0] = '\0';
    g_prov.frame_len = 0;
    g_prov.pending_wifi = false;
    pthread_mutex_unlock(&g_prov.lock);
    syslog(LOG_INFO, "[%s] provisioning window closed\n", TAG);
}

bool ble_provisioning_is_open(void)
{
    bool open;
    pthread_mutex_lock(&g_prov.lock);
    open = g_prov.expires > time(NULL) && g_prov.pin[0] != '\0';
    if (!open) {
        g_prov.expires = 0;
        g_prov.pin[0] = '\0';
        g_prov.pending_wifi = false;
    }
    pthread_mutex_unlock(&g_prov.lock);
    return open;
}

uint32_t ble_provisioning_remaining(void)
{
    time_t now = time(NULL);
    uint32_t remaining = 0;
    pthread_mutex_lock(&g_prov.lock);
    if (g_prov.expires > now)
        remaining = (uint32_t)(g_prov.expires - now);
    pthread_mutex_unlock(&g_prov.lock);
    return remaining;
}

int ble_provisioning_get_pin(char pin[7])
{
    if (!pin || !ble_provisioning_is_open())
        return -ENOENT;
    pthread_mutex_lock(&g_prov.lock);
    memcpy(pin, g_prov.pin, sizeof(g_prov.pin));
    pthread_mutex_unlock(&g_prov.lock);
    return 0;
}

static void handle_status(void)
{
    device_wifi_status_t status;
    char response[192];
    device_wifi_get_status(&status);
    snprintf(response, sizeof(response),
        "{\"v\":1,\"cmd\":\"status\",\"ok\":true,\"network\":%s,\"ip\":\"%s\",\"provisioning\":%s}\n",
        status.state == DEVICE_WIFI_ONLINE ? "true" : "false",
        status.ip[0] ? status.ip : "0.0.0.0",
        ble_provisioning_is_open() ? "true" : "false");
    send_json(response);
}

static void handle_frame(const char *frame)
{
    cJSON *root = cJSON_Parse(frame);
    cJSON *version;
    cJSON *command;
    cJSON *pin;
    cJSON *ssid;
    cJSON *password;
    char expected_pin[7];
    int ret;

    if (!root) {
        send_error("invalid_json");
        return;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "v");
    command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        !cJSON_IsString(command)) {
        cJSON_Delete(root);
        send_error("invalid_request");
        return;
    }

    syslog(LOG_INFO, "[%s] command=%s\n", TAG, command->valuestring);
    if (strcmp(command->valuestring, "ping") == 0) {
        send_json("{\"v\":1,\"cmd\":\"pong\",\"ok\":true}\n");
        cJSON_Delete(root);
        return;
    }
    if (strcmp(command->valuestring, "status") == 0) {
        handle_status();
        cJSON_Delete(root);
        return;
    }
    if (strcmp(command->valuestring, "wifi_config") != 0) {
        cJSON_Delete(root);
        send_error("unknown_command");
        return;
    }
    if (!ble_provisioning_is_open()) {
        cJSON_Delete(root);
        send_error("window_closed");
        return;
    }

    pin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(pin) || !cJSON_IsString(ssid) ||
        (password && !cJSON_IsString(password))) {
        cJSON_Delete(root);
        send_error("invalid_fields");
        return;
    }
    if (ble_provisioning_get_pin(expected_pin) < 0 ||
        strcmp(pin->valuestring, expected_pin) != 0) {
        cJSON_Delete(root);
        send_error("invalid_pin");
        return;
    }

    ret = device_wifi_connect(ssid->valuestring,
        password ? password->valuestring : "");
    if (ret < 0) {
        cJSON_Delete(root);
        send_error(ret == -EINVAL ? "invalid_wifi_fields" : "busy");
        return;
    }
    pthread_mutex_lock(&g_prov.lock);
    g_prov.pending_wifi = true;
    pthread_mutex_unlock(&g_prov.lock);
    send_json("{\"v\":1,\"cmd\":\"wifi_config_result\",\"ok\":true,\"code\":\"accepted\"}\n");
    cJSON_Delete(root);
}

void ble_cmd_handler_recv(const uint8_t *data, uint16_t len, void *user_data)
{
    size_t i;
    (void)user_data;
    if (!data || len == 0)
        return;

    pthread_mutex_lock(&g_prov.lock);
    if (g_prov.frame_len > 0 &&
        time(NULL) - g_prov.frame_updated > BLE_FRAME_TIMEOUT_SEC)
        g_prov.frame_len = 0;

    for (i = 0; i < len; i++) {
        if (data[i] == '\n') {
            char complete[BLE_FRAME_MAX + 1];
            memcpy(complete, g_prov.frame, g_prov.frame_len);
            complete[g_prov.frame_len] = '\0';
            g_prov.frame_len = 0;
            pthread_mutex_unlock(&g_prov.lock);
            handle_frame(complete);
            pthread_mutex_lock(&g_prov.lock);
        } else if (data[i] != '\r') {
            if (g_prov.frame_len >= BLE_FRAME_MAX) {
                g_prov.frame_len = 0;
                pthread_mutex_unlock(&g_prov.lock);
                send_error("frame_too_large");
                return;
            }
            g_prov.frame[g_prov.frame_len++] = (char)data[i];
            g_prov.frame_updated = time(NULL);
        }
    }
    pthread_mutex_unlock(&g_prov.lock);
}
