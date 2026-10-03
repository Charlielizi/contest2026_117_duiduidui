#include "infra/admin_api.h"

#include "agent_config.h"
#include "channels/ws_server.h"
#include "core/session_mgr.h"
#include "device_settings.h"
#include "infra/admin_auth.h"
#include "infra/tls_trust.h"
#include "infra/portal_operations.h"
#include "infra/http_byte_range.h"
#include "infra/agent_logbuf.h"
#include "infra/config_store.h"
#include "labtwin/labtwin.h"
#include "labtwin/labtwin_dashboard.h"
#include "labtwin/labtwin_environment.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"
#include "tools/tool_media.h"
#include "voice/recording_service.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/socket.h>
#include <sys/boardctl.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "infra/portal_chat.h"
#include "mbedtls/sha256.h"
#include <nuttx/board.h>

#define API_BODY_MAX (32 * 1024)
#define SNAPSHOT_BUFFER (96 * 1024)
#define OPERATION_MAX 4
#define REQUEST_CACHE_MAX 16
#define ADVANCED_KVDB_SETTLE_US 250000

typedef enum {
    OP_UNUSED = 0,
    OP_QUEUED,
    OP_RUNNING,
    OP_SUCCEEDED,
    OP_FAILED,
    OP_ROLLED_BACK,
} operation_state_t;

typedef struct {
    bool used;
    unsigned int id;
    char kind[24];
    operation_state_t state;
    char message[96];
    time_t updated;
} operation_t;

typedef struct {
    unsigned int operation_id;
    char ssid[DEVICE_SSID_MAX + 1];
    char password[64];
    char old_ssid[DEVICE_SSID_MAX + 1];
    char old_password[64];
} wifi_job_t;

static pthread_mutex_t g_api_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_request_sequence;
static unsigned long g_revision = 1;
static unsigned int g_operation_sequence;
static operation_t g_operations[OPERATION_MAX];
static char g_request_ids[REQUEST_CACHE_MAX][65];
static unsigned int g_request_id_cursor;

static bool safe_skill_name(const char *name);
static cJSON *response_data(const char *json);

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static int send_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t sent = send(fd, bytes + offset, length - offset, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static const char *header_value(const char *request, const char *name,
                                char *output, size_t size)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;
    snprintf(pattern, sizeof(pattern), "\r\n%s: ", name);
    start = strcasestr(request, pattern);
    if (!start)
        return NULL;
    start += strlen(pattern);
    end = strstr(start, "\r\n");
    if (!end)
        return NULL;
    length = (size_t)(end - start);
    if (length >= size)
        return NULL;
    memcpy(output, start, length);
    output[length] = '\0';
    return output;
}

static void request_id(const char *request, char *output, size_t size)
{
    if (header_value(request, "X-Request-ID", output, size))
        return;
    pthread_mutex_lock(&g_api_lock);
    snprintf(output, size, "board-%lu", ++g_request_sequence);
    pthread_mutex_unlock(&g_api_lock);
}

static void send_envelope(int fd, int code, const char *request,
                          cJSON *data, const char *error_code,
                          const char *error_message,
                          const admin_session_view_t *new_session,
                          bool clear_cookie)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = NULL;
    char id[80];
    char *body;
    char header[768];
    char cookie[192] = { 0 };
    int header_length;
    request_id(request, id, sizeof(id));
    cJSON_AddStringToObject(root, "request_id", id);
    pthread_mutex_lock(&g_api_lock);
    cJSON_AddNumberToObject(root, "revision", (double)g_revision);
    pthread_mutex_unlock(&g_api_lock);
    if (data)
        cJSON_AddItemToObject(root, "data", data);
    else
        cJSON_AddNullToObject(root, "data");
    if (error_code) {
        error = cJSON_AddObjectToObject(root, "error");
        cJSON_AddStringToObject(error, "code", error_code);
        cJSON_AddStringToObject(error, "message",
                                error_message ? error_message : error_code);
    } else {
        cJSON_AddNullToObject(root, "error");
    }
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body)
        return;
    if (new_session && new_session->authenticated)
        snprintf(cookie, sizeof(cookie),
            "Set-Cookie: labtwin_admin=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800\r\n",
            new_session->token);
    else if (clear_cookie)
        snprintf(cookie, sizeof(cookie),
            "Set-Cookie: labtwin_admin=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
    header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        code, status_text(code), (int)strlen(body), cookie);
    send_all(fd, header, (size_t)header_length);
    send_all(fd, body, strlen(body));
    free(body);
}

static char *read_body(int fd, const char *request, int request_len)
{
    const char *header_end = strstr(request, "\r\n\r\n");
    const char *length_header;
    int content_length;
    int available;
    int offset;
    char *body;
    if (!header_end)
        return NULL;
    header_end += 4;
    length_header = strcasestr(request, "\r\nContent-Length: ");
    content_length = length_header ? atoi(length_header + 18) : 0;
    if (content_length <= 0 || content_length > API_BODY_MAX)
        return NULL;
    body = malloc((size_t)content_length + 1);
    if (!body)
        return NULL;
    available = request_len - (int)(header_end - request);
    if (available > content_length) available = content_length;
    if (available > 0) memcpy(body, header_end, (size_t)available);
    offset = available;
    while (offset < content_length) {
        int count = recv(fd, body + offset, (size_t)(content_length - offset), 0);
        if (count <= 0) { free(body); return NULL; }
        offset += count;
    }
    body[content_length] = '\0';
    return body;
}

static cJSON *parse_body(int fd, const char *request, int request_len,
                         char **storage)
{
    *storage = read_body(fd, request, request_len);
    return *storage ? cJSON_Parse(*storage) : NULL;
}

static const char *request_header(const char *request, const char *name,
                                  char *output, size_t size)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;
    snprintf(pattern, sizeof(pattern), "\r\n%s: ", name);
    start = strcasestr(request, pattern);
    if (!start)
        return NULL;
    start += strlen(pattern);
    end = strstr(start, "\r\n");
    if (!end)
        return NULL;
    length = (size_t)(end - start);
    if (length == 0 || length >= size)
        return NULL;
    memcpy(output, start, length);
    output[length] = '\0';
    return output;
}

bool admin_api_origin_valid(const char *request)
{
    char host[96];
    char origin[112];
    char expected[112];
    if (!request_header(request, "Host", host, sizeof(host)) ||
        !request_header(request, "Origin", origin, sizeof(origin)))
        return false;
    snprintf(expected, sizeof(expected), "http://%s", host);
    if (strcmp(origin, expected) == 0)
        return true;
    snprintf(expected, sizeof(expected), "https://%s", host);
    return strcmp(origin, expected) == 0;
}

static bool write_request_is_fresh(const char *request)
{
    char id[65];
    unsigned int i;
    if (!request_header(request, "X-Request-ID", id, sizeof(id)) ||
        strlen(id) < 8)
        return false;
    for (i = 0; id[i]; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_')
            return false;
    pthread_mutex_lock(&g_api_lock);
    for (i = 0; i < REQUEST_CACHE_MAX; i++) {
        if (strcmp(g_request_ids[i], id) == 0) {
            pthread_mutex_unlock(&g_api_lock);
            return false;
        }
    }
    snprintf(g_request_ids[g_request_id_cursor++ % REQUEST_CACHE_MAX], 65,
             "%s", id);
    pthread_mutex_unlock(&g_api_lock);
    return true;
}

static bool require_admin(int fd, const char *request, bool write,
                          admin_session_view_t *session)
{
    if (!admin_auth_session(request, session)) {
        send_envelope(fd, 401, request, NULL, "AUTH_REQUIRED",
                      "administrator login required", NULL, false);
        return false;
    }
    if (write && !admin_auth_csrf_valid(request, session)) {
        send_envelope(fd, 403, request, NULL, "CSRF_INVALID",
                      "request verification failed", NULL, false);
        return false;
    }
    return true;
}

static void audit_event(const char *action, const char *target,
                        const char *result)
{
    /* agent_logbuf already captures syslog in a bounded in-memory ring.  Do
     * not add a second synchronous NAND transaction after an admin setting
     * commit: back-to-back YAFFS writes can stall the R528 request thread. */
    syslog(LOG_INFO, "[admin-audit] actor=admin action=%s target=%s result=%s\n",
           action ? action : "unknown", target ? target : "",
           result ? result : "unknown");
}

static const char *wifi_state_name(device_wifi_state_t state)
{
    switch (state) {
    case DEVICE_WIFI_IDLE: return "idle";
    case DEVICE_WIFI_SCANNING: return "scanning";
    case DEVICE_WIFI_ASSOCIATING: return "associating";
    case DEVICE_WIFI_DHCP: return "dhcp";
    case DEVICE_WIFI_ONLINE: return "online";
    default: return "error";
    }
}

static cJSON *wifi_json(bool include_aps)
{
    device_wifi_status_t status = { 0 };
    cJSON *root = cJSON_CreateObject();
    cJSON *aps = cJSON_AddArrayToObject(root, "aps");
    size_t i;
    device_wifi_get_status(&status);
    cJSON_AddStringToObject(root, "state", wifi_state_name(status.state));
    cJSON_AddStringToObject(root, "ssid", status.ssid);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddStringToObject(root, "gateway", status.gateway);
    cJSON_AddNumberToObject(root, "rssi", status.rssi);
    cJSON_AddNumberToObject(root, "error", status.error);
    if (include_aps) {
        for (i = 0; i < status.ap_count; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", status.aps[i].ssid);
            cJSON_AddStringToObject(item, "bssid", status.aps[i].bssid);
            cJSON_AddNumberToObject(item, "rssi", status.aps[i].rssi);
            cJSON_AddBoolToObject(item, "secure", status.aps[i].secure);
            cJSON_AddItemToArray(aps, item);
        }
    }
    return root;
}

static const char *ble_state_name(device_ble_state_t state)
{
    switch (state) {
    case DEVICE_BLE_OFF: return "off";
    case DEVICE_BLE_IDLE: return "idle";
    case DEVICE_BLE_SCANNING: return "scanning";
    case DEVICE_BLE_PAIRING: return "pairing";
    case DEVICE_BLE_READY: return "ready";
    default: return "error";
    }
}

static cJSON *ble_json(void)
{
    device_ble_status_t status = { 0 };
    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    size_t i;
    device_ble_get_status(&status);
    cJSON_AddStringToObject(root, "state", ble_state_name(status.state));
    cJSON_AddNumberToObject(root, "error", status.error);
    for (i = 0; i < status.device_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", status.devices[i].name);
        cJSON_AddStringToObject(item, "address", status.devices[i].address);
        cJSON_AddNumberToObject(item, "rssi", status.devices[i].rssi);
        cJSON_AddBoolToObject(item, "bonded", status.devices[i].bonded);
        cJSON_AddItemToArray(devices, item);
    }
    return root;
}

static bool handle_ble_action(int fd, const char *request, int request_len,
                              const char *action)
{
    char *body = NULL;
    cJSON *root = NULL;
    int ret = ERROR;
    const char *target = "bluetooth";
    if (strcmp(action, "scan") != 0 && strcmp(action, "enable") != 0 &&
        strcmp(action, "pair") != 0 && strcmp(action, "unpair") != 0) {
        send_envelope(fd, 404, request, NULL, "NOT_FOUND",
                      "bluetooth action not found", NULL, false);
        return true;
    }
    if (strcmp(action, "scan") == 0) {
        ret = device_ble_scan();
    } else {
        root = parse_body(fd, request, request_len, &body);
        if (strcmp(action, "enable") == 0) {
            cJSON *enabled = root ? cJSON_GetObjectItem(root, "enabled") : NULL;
            if (cJSON_IsBool(enabled))
                ret = device_ble_set_enabled(cJSON_IsTrue(enabled));
        } else {
            const char *address = root ? cJSON_GetStringValue(
                cJSON_GetObjectItem(root, "address")) : NULL;
            if (address && strlen(address) == 17) {
                target = address;
                ret = strcmp(action, "pair") == 0 ?
                    device_ble_pair(address) : device_ble_unpair(address);
            }
        }
    }
    cJSON_Delete(root);
    free(body);
    if (ret != OK) {
        send_envelope(fd, 409, request, NULL, "BLUETOOTH_UNAVAILABLE",
                      "bluetooth operation was rejected", NULL, false);
    } else {
        audit_event("bluetooth.change", target, action);
        send_envelope(fd, 202, request, cJSON_CreateObject(),
                      NULL, NULL, NULL, false);
    }
    return true;
}

static bool handle_status(int fd, const char *request)
{
    struct mallinfo memory = mallinfo();
    struct statfs storage;
    cJSON *snapshot = NULL;
    cJSON *device = NULL;
    cJSON *data = cJSON_CreateObject();
    cJSON *wifi = wifi_json(false);
    cJSON *service = cJSON_CreateObject();
    char *buffer = calloc(1, SNAPSHOT_BUFFER);
    char hostname[64] = "labtwin.local";
    unsigned long long total = 0;
    unsigned long long free_bytes = 0;
    double used = 0;
    const char *level = "normal";
    struct timespec uptime = { 0 };
    if (claw_config_get("device.name", hostname, sizeof(hostname)) == OK &&
        !strstr(hostname, ".local"))
        strncat(hostname, ".local", sizeof(hostname) - strlen(hostname) - 1);
    if (buffer && labtwin_dashboard_snapshot_json(buffer, SNAPSHOT_BUFFER) == 0) {
        snapshot = cJSON_Parse(buffer);
        device = snapshot ? cJSON_GetObjectItem(snapshot, "device") : NULL;
    }
    if (statfs("/data", &storage) == 0) {
        total = (unsigned long long)storage.f_blocks * storage.f_bsize;
        free_bytes = (unsigned long long)storage.f_bavail * storage.f_bsize;
        if (total) used = (double)(total - free_bytes) / (double)total;
        if (used >= .95) level = "blocked";
        else if (used >= .85) level = "critical";
        else if (used >= .70) level = "warning";
    }
    const char *device_id = device ? cJSON_GetStringValue(
        cJSON_GetObjectItem(device, "device_id")) : NULL;
    const char *boot_id = device ? cJSON_GetStringValue(
        cJSON_GetObjectItem(device, "boot_id")) : NULL;
    clock_gettime(CLOCK_MONOTONIC, &uptime);
    cJSON_AddStringToObject(data, "device_id", device_id ? device_id : AGENT_NODE_ID);
    cJSON_AddStringToObject(data, "hostname", hostname);
    cJSON_AddStringToObject(data, "lan_hostname", "labtwin.lan");
    cJSON_AddStringToObject(data, "firmware", AGENT_BUILD_VERSION);
    cJSON_AddStringToObject(data, "tls_status", tls_trust_status());
    cJSON_AddStringToObject(data, "boot_id", boot_id ? boot_id : "unknown");
    cJSON_AddNumberToObject(data, "uptime_seconds", (double)uptime.tv_sec);
    cJSON_AddNumberToObject(data, "heap_total_bytes", memory.arena);
    cJSON_AddNumberToObject(data, "heap_used_bytes", memory.uordblks);
    cJSON_AddNumberToObject(data, "heap_free_bytes", memory.fordblks);
    cJSON_AddNumberToObject(data, "heap_largest_bytes", memory.mxordblk);
    cJSON_AddNumberToObject(data, "storage_total_bytes", (double)total);
    cJSON_AddNumberToObject(data, "storage_free_bytes", (double)free_bytes);
    cJSON_AddStringToObject(data, "storage_level", level);
    cJSON_AddItemToObject(data, "wifi", wifi);
    cJSON_AddStringToObject(service, "state", "running");
    cJSON_AddNumberToObject(service, "clients", ws_server_client_count());
    cJSON_AddItemToObject(data, "service", service);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    cJSON_Delete(snapshot);
    free(buffer);
    return true;
}

static bool safe_experiment_id(const char *id)
{
    size_t i;
    if (!id || !id[0] || strlen(id) > 31 || strstr(id, ".."))
        return false;
    for (i = 0; id[i]; i++)
        if (!(isalnum((unsigned char)id[i]) || id[i] == '-' || id[i] == '_'))
            return false;
    return true;
}

static bool handle_experiment_detail(int fd, const char *request,
                                     const char *experiment_id)
{
    char input[96];
    char *output;
    cJSON *data;
    if (!safe_experiment_id(experiment_id)) {
        send_envelope(fd, 404, request, NULL, "EXPERIMENT_NOT_FOUND",
                      "experiment does not exist", NULL, false);
        return true;
    }
    output = calloc(1, 16 * 1024);
    if (!output) {
        send_envelope(fd, 503, request, NULL, "MEMORY_UNAVAILABLE",
                      "experiment detail memory unavailable", NULL, false);
        return true;
    }
    snprintf(input, sizeof(input), "{\"experiment_id\":\"%s\"}", experiment_id);
    if (labtwin_experiment_get_json(input, output, 16 * 1024) != 0 ||
        !(data = response_data(output))) {
        free(output);
        send_envelope(fd, 404, request, NULL, "EXPERIMENT_NOT_FOUND",
                      "experiment does not exist", NULL, false);
        return true;
    }
    free(output);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

typedef int (*labtwin_operation_t)(const char *, char *, size_t, const char *);

static bool send_labtwin_result(int fd, const char *request, char *output,
                                int result, const char *audit_action)
{
    cJSON *root = cJSON_Parse(output);
    cJSON *ok = root ? cJSON_GetObjectItem(root, "ok") : NULL;
    cJSON *code = root ? cJSON_GetObjectItem(root, "code") : NULL;
    cJSON *message = root ? cJSON_GetObjectItem(root, "message") : NULL;
    cJSON *data = root ? cJSON_DetachItemFromObject(root, "data") : NULL;
    const char *code_text = cJSON_GetStringValue(code);
    const char *message_text = cJSON_GetStringValue(message);
    int status = 400;
    if (result == 0 && cJSON_IsTrue(ok)) {
        pthread_mutex_lock(&g_api_lock);
        g_revision++;
        pthread_mutex_unlock(&g_api_lock);
        audit_event(audit_action, "experiment", "success");
        send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    } else {
        if (code_text && strcmp(code_text, "NOT_FOUND") == 0) status = 404;
        else if (code_text && strcmp(code_text, "CONFLICT") == 0) status = 409;
        else if (code_text && (strcmp(code_text, "STORAGE_ERROR") == 0 ||
                               strcmp(code_text, "STORAGE_FULL") == 0)) status = 503;
        audit_event(audit_action, "experiment", "failed");
        send_envelope(fd, status, request, data, code_text ? code_text : "OPERATION_FAILED",
                      message_text ? message_text : "experiment operation failed", NULL, false);
    }
    cJSON_Delete(root);
    return true;
}

static bool handle_experiment_operation(int fd, const char *request,
                                        int request_len,
                                        labtwin_operation_t operation,
                                        const char *audit_action)
{
    char *body = read_body(fd, request, request_len);
    char *output;
    int result;
    if (!body) {
        send_envelope(fd, 400, request, NULL, "INVALID_ARGUMENT",
                      "JSON request body required", NULL, false);
        return true;
    }
    output = calloc(1, 16 * 1024);
    if (!output) {
        memset(body, 0, strlen(body)); free(body);
        send_envelope(fd, 503, request, NULL, "MEMORY_UNAVAILABLE",
                      "operation memory unavailable", NULL, false);
        return true;
    }
    result = operation(body, output, 16 * 1024, "portal");
    memset(body, 0, strlen(body));
    free(body);
    send_labtwin_result(fd, request, output, result, audit_action);
    free(output);
    return true;
}

static bool query_value(const char *query, const char *name, char *out,
                        size_t out_size)
{
    const char *item = query && *query == '?' ? query + 1 : query;
    size_t name_len = strlen(name);
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    while (item && *item) {
        const char *end = strchr(item, '&');
        const char *equals = strchr(item, '=');
        size_t encoded_len;
        size_t written = 0;
        if (!end) end = item + strlen(item);
        if (equals && equals < end && (size_t)(equals - item) == name_len &&
            strncmp(item, name, name_len) == 0) {
            const char *value = equals + 1;
            encoded_len = (size_t)(end - value);
            while (encoded_len-- && written + 1 < out_size) {
                unsigned char ch = (unsigned char)*value++;
                if (ch == '+' ) ch = ' ';
                else if (ch == '%' && encoded_len >= 2 &&
                         isxdigit((unsigned char)value[0]) &&
                         isxdigit((unsigned char)value[1])) {
                    char hex[3] = { value[0], value[1], '\0' };
                    ch = (unsigned char)strtoul(hex, NULL, 16);
                    value += 2;
                    encoded_len -= 2;
                }
                out[written++] = (char)ch;
            }
            out[written] = '\0';
            return true;
        }
        item = *end ? end + 1 : NULL;
    }
    return false;
}

static bool query_number(cJSON *data, const char *query, const char *name)
{
    char value[32];
    char *end;
    long long number;
    if (!query_value(query, name, value, sizeof(value))) return true;
    errno = 0;
    number = strtoll(value, &end, 10);
    if (errno || !value[0] || *end) return false;
    cJSON_AddNumberToObject(data, name, (double)number);
    return true;
}

static bool handle_experiment_history(int fd, const char *request,
                                      const char *query)
{
    char *output;
    char value[513];
    char *input;
    cJSON *data;
    cJSON *filters = cJSON_CreateObject();
    if (!filters) {
        send_envelope(fd, 503, request, NULL, "MEMORY_UNAVAILABLE",
                      "history query memory unavailable", NULL, false);
        return true;
    }
    if (query_value(query, "state", value, sizeof(value)) && value[0])
        cJSON_AddStringToObject(filters, "state", value);
    if (query_value(query, "query", value, sizeof(value)) && value[0])
        cJSON_AddStringToObject(filters, "query", value);
    if (!query_number(filters, query, "from_epoch") ||
        !query_number(filters, query, "to_epoch") ||
        !query_number(filters, query, "offset") ||
        !query_number(filters, query, "limit")) {
        cJSON_Delete(filters);
        send_envelope(fd, 400, request, NULL, "INVALID_ARGUMENT",
                      "history query parameters are invalid", NULL, false);
        return true;
    }
    input = cJSON_PrintUnformatted(filters);
    cJSON_Delete(filters);
    output = input ? calloc(1, SNAPSHOT_BUFFER) : NULL;
    if (!input || !output || labtwin_experiment_history_json(input, output,
                                        SNAPSHOT_BUFFER) != 0 ||
        !(data = response_data(output))) {
        free(input);
        free(output);
        send_envelope(fd, 503, request, NULL, "HISTORY_UNAVAILABLE",
                      "experiment history unavailable", NULL, false);
        return true;
    }
    free(input);
    free(output);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool handle_portal_history(int fd, const char *request)
{
    char *history = calloc(1, 32 * 1024);
    cJSON *data;
    cJSON *messages;
    if (!history || session_get_history_json("portal-admin", history,
                                             32 * 1024, 100) != 0 ||
        !(messages = cJSON_Parse(history))) {
        free(history);
        send_envelope(fd, 503, request, NULL, "CHAT_HISTORY_UNAVAILABLE",
                      "portal conversation unavailable", NULL, false);
        return true;
    }
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "chat_id", "portal-admin");
    cJSON_AddItemToObject(data, "messages", messages);
    free(history);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool handle_portal_history_clear(int fd, const char *request)
{
    int ret = portal_chat_clear();
    if (ret != 0) {
        send_envelope(fd, ret == -EBUSY ? 409 : 503, request, NULL,
                      ret == -EBUSY ? "CHAT_BUSY" : "CHAT_HISTORY_CLEAR_FAILED",
                      ret == -EBUSY ? "请等待当前请求结束后再清空，避免在途回复重新出现" : "portal conversation could not be cleared", NULL, false);
        return true;
    }
    audit_event("portal.chat.clear", "portal-admin", "success");
    send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    return true;
}

static bool path_ends_with(const char *value, const char *suffix)
{
    size_t value_len = value ? strlen(value) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool handle_recordings_get(int fd, const char *request)
{
    send_envelope(fd, 200, request, recording_service_snapshot_json(),
                  NULL, NULL, NULL, false);
    return true;
}

static bool handle_recordings_start(int fd, const char *request)
{
    char id[16];
    int ret = recording_service_start(id, sizeof(id));
    if (ret == 0) {
        audit_event("recording.start", id, "success");
        send_envelope(fd, 201, request, recording_service_snapshot_json(),
                      NULL, NULL, NULL, false);
    } else if (ret == -ENOSPC) {
        audit_event("recording.start", "storage", "rejected");
        send_envelope(fd, 409, request, NULL, "RECORDING_STORAGE_LIMIT",
                      "not enough storage remains for a recording", NULL, false);
    } else if (ret == -EBUSY) {
        send_envelope(fd, 409, request, NULL, "AUDIO_BUSY",
                      "microphone is currently in use", NULL, false);
    } else {
        audit_event("recording.start", "microphone", "failed");
        send_envelope(fd, 503, request, NULL, "RECORDING_UNAVAILABLE",
                      "microphone recording could not start", NULL, false);
    }
    return true;
}

static bool handle_recordings_stop(int fd, const char *request, const char *id)
{
    int ret = recording_service_stop(id);
    if (ret == 0) {
        audit_event("recording.stop", id, "success");
        send_envelope(fd, 200, request, recording_service_snapshot_json(),
                      NULL, NULL, NULL, false);
    } else if (ret == -ENOENT) {
        send_envelope(fd, 404, request, NULL, "RECORDING_NOT_ACTIVE",
                      "recording is not active", NULL, false);
    } else if (ret == -EBUSY) {
        send_envelope(fd, 409, request, NULL, "RECORDING_BUSY",
                      "recording is starting or finalizing; check its status", NULL, false);
    } else if (ret != -EINVAL) {
        send_envelope(fd, 503, request, NULL, "RECORDING_FINALIZATION_FAILED",
                      "recording could not be published; temporary audio has been preserved", NULL, false);
    } else {
        send_envelope(fd, 400, request, NULL, "RECORDING_ID_INVALID",
                      "recording identifier is invalid", NULL, false);
    }
    return true;
}

static bool handle_recordings_delete(int fd, const char *request, const char *id)
{
    int ret = recording_service_delete(id);
    if (ret == 0) {
        audit_event("recording.delete", id, "success");
        send_envelope(fd, 200, request, recording_service_snapshot_json(),
                      NULL, NULL, NULL, false);
    } else if (ret == -EBUSY) {
        send_envelope(fd, 409, request, NULL, "RECORDING_ACTIVE",
                      "stop the recording before deleting it", NULL, false);
    } else if (ret == -ENOENT) {
        send_envelope(fd, 404, request, NULL, "RECORDING_NOT_FOUND",
                      "recording was not found", NULL, false);
    } else {
        send_envelope(fd, 400, request, NULL, "RECORDING_ID_INVALID",
                      "recording identifier is invalid", NULL, false);
    }
    return true;
}

static bool send_recording_audio(int fd, const char *request, const char *id)
{
    char path[128];
    char range[96];
    char header[512];
    unsigned char buffer[4096];
    unsigned long long file_size;
    unsigned long long start = 0;
    unsigned long long end;
    unsigned long long length;
    bool partial = false;
    int audio_fd;
    int header_length;

    if (recording_service_resolve_audio(id, path, sizeof(path), &file_size) < 0) {
        send_envelope(fd, 404, request, NULL, "RECORDING_NOT_FOUND",
                      "recording was not found", NULL, false);
        return true;
    }
    int range_ret = http_byte_range(header_value(request, "Range", range, sizeof(range)) ? range : NULL,
                                   file_size, &start, &end);
    if (range_ret < 0) {
        if (range_ret == -ERANGE) {
            header_length = snprintf(header, sizeof(header), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", file_size);
            send_all(fd, header, (size_t)header_length);
        } else send_envelope(fd, 400, request, NULL, "RANGE_INVALID", "invalid or unsupported byte range", NULL, false);
        return true;
    }
    partial = range_ret == 1;
    length = end - start + 1;
    audio_fd = open(path, O_RDONLY);
    if (audio_fd < 0 || lseek(audio_fd, (off_t)start, SEEK_SET) < 0) {
        if (audio_fd >= 0) close(audio_fd);
        send_envelope(fd, 500, request, NULL, "RECORDING_READ_FAILED",
                      "recording could not be read", NULL, false);
        return true;
    }
    if (partial) {
        header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: audio/wav\r\nContent-Length: %llu\r\n"
            "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\n"
            "Content-Disposition: inline; filename=\"%s.wav\"\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
            length, start, end, file_size, id);
    } else {
        header_length = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
            "Content-Length: %llu\r\nAccept-Ranges: bytes\r\n"
            "Content-Disposition: inline; filename=\"%s.wav\"\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n", length, id);
    }
    if (send_all(fd, header, (size_t)header_length) == 0) {
        unsigned long long remaining = length;
        while (remaining) {
            size_t wanted = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
            ssize_t count = read(audio_fd, buffer, wanted);
            if (count <= 0 || send_all(fd, buffer, (size_t)count) != 0) break;
            remaining -= (unsigned long long)count;
        }
    }
    close(audio_fd);
    return true;
}

static int file_sha256(const char *path, char output[65], long *size_out)
{
    unsigned char digest[32];
    unsigned char buffer[2048];
    mbedtls_sha256_context context;
    FILE *file = fopen(path, "rb");
    size_t count;
    long size = 0;
    static const char digits[] = "0123456789abcdef";
    int i;
    if (!file)
        return -1;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        mbedtls_sha256_update(&context, buffer, count);
        size += (long)count;
    }
    fclose(file);
    mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    for (i = 0; i < 32; i++) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 15];
    }
    output[64] = '\0';
    *size_out = size;
    return 0;
}

static int remove_experiment_files(const char *id, void *context)
{
    char directory[160], snapshot_path[192], events_path[192], deletion_path[224];
    char snapshot_hash[65] = "missing", events_hash[65] = "missing";
    long snapshot_size = 0, events_size = 0;
    cJSON *manifest;
    char *json;
    FILE *file;
    int result;
    snprintf(directory, sizeof(directory), "/data/labtwin/experiments/%s", id);
    snprintf(snapshot_path, sizeof(snapshot_path), "%s/snapshot.json", directory);
    snprintf(events_path, sizeof(events_path), "%s/events.jsonl", directory);
    if (access(directory, F_OK) != 0) return -ENOENT;
    file_sha256(snapshot_path, snapshot_hash, &snapshot_size);
    file_sha256(events_path, events_hash, &events_size);
    mkdir("/data/labtwin/deletions", 0700);
    snprintf(deletion_path, sizeof(deletion_path),
             "/data/labtwin/deletions/%s-%lld.json", id,
             (long long)time(NULL));
    manifest = cJSON_CreateObject();
    cJSON_AddNumberToObject(manifest, "schema_version", 1);
    cJSON_AddStringToObject(manifest, "experiment_id", id);
    cJSON_AddStringToObject(manifest, "source", context ? (const char *)context : "portal");
    cJSON_AddNumberToObject(manifest, "deleted_epoch", (double)time(NULL));
    cJSON *files = cJSON_AddArrayToObject(manifest, "files");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", "snapshot.json");
    cJSON_AddNumberToObject(item, "size", snapshot_size);
    cJSON_AddStringToObject(item, "sha256", snapshot_hash);
    cJSON_AddItemToArray(files, item);
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "name", "events.jsonl");
    cJSON_AddNumberToObject(item, "size", events_size);
    cJSON_AddStringToObject(item, "sha256", events_hash);
    cJSON_AddItemToArray(files, item);
    json = cJSON_PrintUnformatted(manifest);
    cJSON_Delete(manifest);
    file = json ? fopen(deletion_path, "w") : NULL;
    if (!file || fwrite(json, 1, strlen(json), file) != strlen(json)) {
        if (file) {
            fclose(file);
        }
        free(json);
        return -EIO;
    }
    if (fflush(file) || fsync(fileno(file))) { fclose(file); free(json); return -EIO; }
    if (fclose(file)) { free(json); return -EIO; }
    free(json); chmod(deletion_path, 0600);
    result = 0;
    if (unlink(events_path) != 0 && errno != ENOENT) result = -1;
    if (unlink(snapshot_path) != 0 && errno != ENOENT) result = -1;
    if (rmdir(directory) != 0) result = -1;
    return result ? -EIO : 0;
}

int admin_delete_experiment(const char *id, uint64_t sequence, const char *source)
{
    int result = labtwin_experiment_delete_locked(id, sequence, remove_experiment_files, (void *)source);
    audit_event("experiment.delete", id, result == 0 ? "success" : "failed-or-partial");
    return result;
}

static bool handle_experiment_delete(int fd, const char *request, int request_len, const char *id)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    const char *confirm = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "confirm_experiment_id")) : NULL;
    const char *password = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "password")) : NULL;
    bool allowed = safe_experiment_id(id) && confirm && !strcmp(confirm, id) &&
        (admin_auth_private_open() || admin_auth_verify_password(password));
    cJSON_Delete(root);
    if (body) { memset(body, 0, strlen(body)); free(body); }
    if (!allowed) {
        send_envelope(fd, 403, request, NULL, "REAUTH_FAILED",
                      "exact experiment id and public-mode password required", NULL, false);
        return true;
    }
    int result = admin_delete_experiment(id, 0, "portal");
    if (result)
        send_envelope(fd, result == -ENOENT ? 404 : 500, request, NULL, "DELETE_FAILED",
                      "deletion failed or incomplete; inspect saved manifest", NULL, false);
    else {
        pthread_mutex_lock(&g_api_lock); g_revision++; pthread_mutex_unlock(&g_api_lock);
        send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    }
    return true;
}

static bool handle_portal_operations(int fd, const char *request, int request_len,
                                     const char *method, const char *tail,
                                     const admin_session_view_t *session)
{
    char id[33] = "";
    const char *suffix = strchr(tail, '/');
    size_t len = suffix ? (size_t)(suffix - tail) : strlen(tail);
    if (len > 32) {
        send_envelope(fd, 400, request, NULL, "INVALID_ARGUMENT", "invalid operation id", NULL, false);
        return true;
    }
    memcpy(id, tail, len);
    if (!strcmp(method, "GET") && !suffix) {
        cJSON *data = portal_operations_get(session->token, id[0] ? id : NULL);
        if (!data) send_envelope(fd, 404, request, NULL, "OPERATION_UNAVAILABLE", "operation unavailable or ledger needs recovery", NULL, false);
        else send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
        return true;
    }
    if (!strcmp(method, "POST") && suffix && (!strcmp(suffix, "/confirm") || !strcmp(suffix, "/cancel"))) {
        char *body = NULL;
        cJSON *root = parse_body(fd, request, request_len, &body), *data = NULL;
        int ret = root ? portal_operation_resolve(session->token, id, root, !strcmp(suffix, "/cancel"), &data) : -EINVAL;
        cJSON_Delete(root);
        if (body) { memset(body, 0, strlen(body)); free(body); }
        if (ret) send_envelope(fd, ret == -EACCES ? 403 : 503, request, NULL, "CONFIRMATION_REJECTED", "confirmation rejected; check token, expiry, id or password", NULL, false);
        else {
            audit_event("portal-agent.confirmation", id, "resolved");
            send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
        }
        return true;
    }
    send_envelope(fd, 400, request, NULL, "INVALID_ARGUMENT", "invalid operation route", NULL, false);
    return true;
}

static bool handle_snapshot(int fd, const char *request)
{
    char *buffer = calloc(1, SNAPSHOT_BUFFER);
    cJSON *data;
    if (!buffer || labtwin_dashboard_snapshot_json(buffer, SNAPSHOT_BUFFER) != 0) {
        free(buffer);
        send_envelope(fd, 503, request, NULL, "SNAPSHOT_FAILED",
                      "dashboard snapshot unavailable", NULL, false);
        return true;
    }
    data = cJSON_Parse(buffer);
    free(buffer);
    if (!data) {
        send_envelope(fd, 500, request, NULL, "SNAPSHOT_INVALID",
                      "dashboard snapshot is invalid", NULL, false);
        return true;
    }
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool handle_auth_state(int fd, const char *request)
{
    admin_session_view_t session;
    bool authenticated = admin_auth_session(request, &session);
    bool initialized = admin_auth_is_initialized();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "initialized", initialized);
    /* Keep the old field for browser clients that have not yet reloaded. */
    cJSON_AddBoolToObject(data, "paired", initialized);
    cJSON_AddBoolToObject(data, "authenticated", authenticated);
    cJSON_AddBoolToObject(data, "private_open", authenticated && admin_auth_private_open());
    cJSON_AddStringToObject(data, "role", authenticated ? "admin" : "guest");
    if (authenticated)
        cJSON_AddStringToObject(data, "csrf_token", session.csrf);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool handle_auth_submit(int fd, const char *request, int request_len,
                               bool initial_setup)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    const char *password = root ? cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "password")) : NULL;
    admin_session_view_t session = { 0 };
    int ret = initial_setup ? admin_auth_initialize(password, &session) :
                              admin_auth_login(password, &session);
    cJSON_Delete(root);
    if (body) { memset(body, 0, strlen(body)); free(body); }
    if (ret != 0) {
        const char *error = initial_setup ? "INITIAL_SETUP_FAILED" : "LOGIN_FAILED";
        const char *message = initial_setup ? "administrator password could not be initialized" :
                                              "password is invalid or login is temporarily limited";
        int status = initial_setup ? 403 : 401;

        if (initial_setup && ret == -EINVAL) {
            error = "ADMIN_PASSWORD_INVALID";
            message = "administrator password must contain 6 to 128 digits";
        } else if (initial_setup && ret == -EACCES) {
            error = "ADMIN_ALREADY_INITIALIZED";
            message = "administrator password has already been initialized";
        } else if (initial_setup && ret == -ENOSPC) {
            error = "CREDENTIAL_STORAGE_UNAVAILABLE";
            message = "board credential storage is unavailable; retry after rebooting the board";
            status = 503;
        } else if (initial_setup && ret == -EIO) {
            error = "AUTH_CRYPTO_UNAVAILABLE";
            message = "board security service is unavailable; retry after rebooting the board";
            status = 503;
        }
        send_envelope(fd, status, request, NULL, error, message, NULL, false);
        audit_event(initial_setup ? "auth.setup" : "auth.login", "admin", "failed");
        return true;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "initialized", true);
    cJSON_AddBoolToObject(data, "paired", true);
    cJSON_AddBoolToObject(data, "authenticated", true);
    cJSON_AddStringToObject(data, "role", "admin");
    cJSON_AddStringToObject(data, "csrf_token", session.csrf);
    send_envelope(fd, initial_setup ? 201 : 200, request, data, NULL, NULL,
                  &session, false);
    audit_event(initial_setup ? "auth.setup" : "auth.login", "admin", "success");
    return true;
}

static cJSON *operation_json(const operation_t *operation)
{
    const char *state = "queued";
    cJSON *data = cJSON_CreateObject();
    if (operation->state == OP_RUNNING) state = "running";
    else if (operation->state == OP_SUCCEEDED) state = "succeeded";
    else if (operation->state == OP_FAILED) state = "failed";
    else if (operation->state == OP_ROLLED_BACK) state = "rolled_back";
    cJSON_AddNumberToObject(data, "operation_id", operation->id);
    cJSON_AddStringToObject(data, "kind", operation->kind);
    cJSON_AddStringToObject(data, "state", state);
    cJSON_AddStringToObject(data, "message", operation->message);
    cJSON_AddNumberToObject(data, "updated_epoch", (double)operation->updated);
    return data;
}

static operation_t *operation_create(const char *kind)
{
    operation_t *slot = NULL;
    int i;
    pthread_mutex_lock(&g_api_lock);
    for (i = 0; i < OPERATION_MAX; i++)
        if (!g_operations[i].used) { slot = &g_operations[i]; break; }
    if (!slot) {
        slot = &g_operations[0];
        for (i = 1; i < OPERATION_MAX; i++)
            if (g_operations[i].updated < slot->updated) slot = &g_operations[i];
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->id = ++g_operation_sequence;
    slot->state = OP_QUEUED;
    slot->updated = time(NULL);
    snprintf(slot->kind, sizeof(slot->kind), "%s", kind);
    snprintf(slot->message, sizeof(slot->message), "queued");
    pthread_mutex_unlock(&g_api_lock);
    return slot;
}

static void operation_update(unsigned int id, operation_state_t state,
                             const char *message)
{
    int i;
    pthread_mutex_lock(&g_api_lock);
    for (i = 0; i < OPERATION_MAX; i++) {
        if (g_operations[i].used && g_operations[i].id == id) {
            g_operations[i].state = state;
            g_operations[i].updated = time(NULL);
            snprintf(g_operations[i].message, sizeof(g_operations[i].message),
                     "%s", message ? message : "");
            break;
        }
    }
    pthread_mutex_unlock(&g_api_lock);
}

static void *wifi_worker(void *arg)
{
    wifi_job_t *job = arg;
    device_wifi_status_t status = { 0 };
    int elapsed;
    operation_update(job->operation_id, OP_RUNNING, "switching network");
    sleep(3);
    if (device_wifi_connect(job->ssid, job->password) != OK) {
        operation_update(job->operation_id, OP_FAILED, "wifi request rejected");
        goto done;
    }
    for (elapsed = 0; elapsed < 30; elapsed++) {
        sleep(1);
        if (device_wifi_get_status(&status) == OK &&
            status.state == DEVICE_WIFI_ONLINE &&
            strcmp(status.ssid, job->ssid) == 0) {
            operation_update(job->operation_id, OP_SUCCEEDED, status.ip);
            audit_event("network.connect", job->ssid, "success");
            goto done;
        }
    }
    if (job->old_ssid[0]) {
        device_wifi_connect(job->old_ssid, job->old_password);
        operation_update(job->operation_id, OP_ROLLED_BACK,
                         "new network failed; previous network restored");
        audit_event("network.connect", job->ssid, "rolled_back");
    } else {
        operation_update(job->operation_id, OP_FAILED, "network timeout");
        audit_event("network.connect", job->ssid, "failed");
    }
done:
    memset(job, 0, sizeof(*job));
    free(job);
    return NULL;
}

static bool handle_network_connect(int fd, const char *request, int request_len)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    const char *ssid = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid")) : NULL;
    const char *password = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "password")) : NULL;
    wifi_job_t *job;
    operation_t *operation;
    pthread_t thread;
    pthread_attr_t attr;
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > DEVICE_SSID_MAX ||
        (password && strlen(password) > 63)) {
        cJSON_Delete(root); if (body) free(body);
        send_envelope(fd, 400, request, NULL, "INVALID_WIFI",
                      "SSID or password length is invalid", NULL, false);
        return true;
    }
    job = calloc(1, sizeof(*job));
    operation = operation_create("wifi_connect");
    if (!job || !operation) {
        free(job); cJSON_Delete(root); if (body) free(body);
        send_envelope(fd, 500, request, NULL, "OUT_OF_MEMORY",
                      "cannot create network operation", NULL, false);
        return true;
    }
    job->operation_id = operation->id;
    snprintf(job->ssid, sizeof(job->ssid), "%s", ssid);
    snprintf(job->password, sizeof(job->password), "%s", password ? password : "");
    claw_config_get(AGENT_CFG_KEY_WIFI_SSID, job->old_ssid, sizeof(job->old_ssid));
    claw_config_get(AGENT_CFG_KEY_WIFI_PASS, job->old_password, sizeof(job->old_password));
    cJSON_Delete(root); if (body) { memset(body, 0, strlen(body)); free(body); }
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 4096);
    if (pthread_create(&thread, &attr, wifi_worker, job) != 0) {
        pthread_attr_destroy(&attr); free(job);
        operation_update(operation->id, OP_FAILED, "worker unavailable");
        send_envelope(fd, 500, request, NULL, "WORKER_FAILED",
                      "cannot start network operation", NULL, false);
        return true;
    }
    pthread_attr_destroy(&attr);
    send_envelope(fd, 202, request, operation_json(operation), NULL, NULL,
                  NULL, false);
    return true;
}

static cJSON *response_data(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *data = root ? cJSON_DetachItemFromObject(root, "data") : NULL;
    cJSON_Delete(root);
    return data;
}

static bool handle_environment_get(int fd, const char *request)
{
    /* This runs on the 12 KiB WebSocket client stack.  Four fixed rules fit
     * comfortably in 1 KiB; a larger local buffer risks overflowing the
     * request thread once cJSON and response frames are included. */
    char output[1024] = { 0 };
    cJSON *data;
    if (labtwin_environment_rule_show_json(output, sizeof(output)) != 0 ||
        !(data = response_data(output))) {
        send_envelope(fd, 500, request, NULL, "RULES_FAILED",
                      "environment rules unavailable", NULL, false);
        return true;
    }
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool valid_rule(cJSON *rule)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(rule, "rule_id"));
    cJSON *trigger = cJSON_GetObjectItem(rule, "trigger");
    cJSON *clear = cJSON_GetObjectItem(rule, "clear");
    if (!id || !cJSON_IsNumber(trigger) || !cJSON_IsNumber(clear)) return false;
    if (strcmp(id, "TEMP_HIGH") == 0 || strcmp(id, "HUMIDITY_HIGH") == 0)
        return trigger->valuedouble > clear->valuedouble;
    if (strcmp(id, "TEMP_LOW") == 0 || strcmp(id, "HUMIDITY_LOW") == 0)
        return trigger->valuedouble < clear->valuedouble;
    return false;
}

static bool handle_environment_put(int fd, const char *request, int request_len)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    cJSON *rules = root ? cJSON_GetObjectItem(root, "rules") : NULL;
    int i;
    char output[1024];
    if (!cJSON_IsArray(rules) || cJSON_GetArraySize(rules) != 4) {
        cJSON_Delete(root); free(body);
        send_envelope(fd, 400, request, NULL, "INVALID_RULES",
                      "exactly four valid environment rules are required", NULL, false);
        return true;
    }
    for (i = 0; i < 4; i++) {
        if (!valid_rule(cJSON_GetArrayItem(rules, i))) {
            cJSON_Delete(root); free(body);
            send_envelope(fd, 400, request, NULL, "INVALID_RULES",
                          "trigger and clear thresholds are inconsistent", NULL, false);
            return true;
        }
    }
    /* Validate, apply and persist the complete rule document under one lock.
     * The bulk API also restores all in-memory values if persistence fails. */
    if (labtwin_environment_rules_set_json(body, output, sizeof(output)) != 0) {
        cJSON_Delete(root); free(body);
        send_envelope(fd, 500, request, NULL, "RULES_NOT_SAVED",
                      "environment rules were not persisted", NULL, false);
        return true;
    }
    cJSON_Delete(root); free(body);
    pthread_mutex_lock(&g_api_lock); g_revision++; pthread_mutex_unlock(&g_api_lock);
    audit_event("settings.environment", "rules", "success");
    return handle_environment_get(fd, request);
}

static bool handle_skills_get_v2(int fd, const char *request)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(data, "skills");
    DIR *directory = opendir(AGENT_SKILLS_DIR);
    struct dirent *entry;
    while (directory && (entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        char path[256];
        struct stat st;
        cJSON *skill;
        if (length < 4 || strcmp(entry->d_name + length - 3, ".md") != 0)
            continue;
        snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR, entry->d_name);
        if (stat(path, &st) != 0) continue;
        skill = cJSON_CreateObject();
        char name[128];
        snprintf(name, sizeof(name), "%.*s", (int)(length - 3), entry->d_name);
        cJSON_AddStringToObject(skill, "name", name);
        cJSON_AddStringToObject(skill, "description", "");
        cJSON_AddNumberToObject(skill, "size", (double)st.st_size);
        cJSON_AddNumberToObject(skill, "mtime", (double)st.st_mtime);
        cJSON_AddItemToArray(array, skill);
    }
    if (directory) closedir(directory);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static void skill_revision(const struct stat *st, char *out, size_t out_size)
{
    snprintf(out, out_size, "%lu-%lu", (unsigned long)st->st_mtime,
             (unsigned long)st->st_size);
}

static bool handle_skill_get_v2(int fd, const char *request, const char *name)
{
    char path[256];
    char revision[48];
    struct stat st;
    FILE *file;
    char *content;
    size_t count;
    cJSON *data;
    if (!safe_skill_name(name)) {
        send_envelope(fd, 400, request, NULL, "INVALID_SKILL",
                      "skill name is invalid", NULL, false);
        return true;
    }
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, name);
    if (stat(path, &st) != 0 || st.st_size < 0 || st.st_size > API_BODY_MAX) {
        send_envelope(fd, 404, request, NULL, "SKILL_NOT_FOUND",
                      "skill does not exist", NULL, false);
        return true;
    }
    file = fopen(path, "r");
    content = malloc((size_t)st.st_size + 1);
    if (!file || !content) {
        if (file) fclose(file);
        free(content);
        send_envelope(fd, 500, request, NULL, "SKILL_READ_FAILED",
                      "skill could not be read", NULL, false);
        return true;
    }
    count = fread(content, 1, (size_t)st.st_size, file);
    fclose(file);
    if (count != (size_t)st.st_size) {
        free(content);
        send_envelope(fd, 500, request, NULL, "SKILL_READ_FAILED",
                      "skill could not be read", NULL, false);
        return true;
    }
    content[count] = '\0';
    skill_revision(&st, revision, sizeof(revision));
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", name);
    cJSON_AddStringToObject(data, "content", content);
    cJSON_AddStringToObject(data, "revision", revision);
    cJSON_AddNumberToObject(data, "size", (double)st.st_size);
    cJSON_AddNumberToObject(data, "mtime", (double)st.st_mtime);
    free(content);
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

static bool handle_skill_delete_v2(int fd, const char *request,
                                   const char *name)
{
    char path[256];
    if (!safe_skill_name(name)) {
        send_envelope(fd, 400, request, NULL, "INVALID_SKILL",
                      "skill name is invalid", NULL, false);
        return true;
    }
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, name);
    if (unlink(path) != 0 && errno != ENOENT) {
        send_envelope(fd, 500, request, NULL, "SKILL_DELETE_FAILED",
                      "skill could not be deleted", NULL, false);
        return true;
    }
    skill_loader_refresh();
    audit_event("skill.delete", name, "success");
    send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    return true;
}

static bool safe_skill_name(const char *name)
{
    size_t i;
    if (!name || !name[0] || strlen(name) > 64 || strstr(name, "..")) return false;
    for (i = 0; name[i]; i++)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '-' || name[i] == '_'))
            return false;
    return true;
}

static bool handle_skills_post_v2(int fd, const char *request, int request_len)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    const char *name = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")) : NULL;
    const char *content = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "content")) : NULL;
    const char *expected_revision = root ? cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "revision")) : NULL;
    char path[256];
    char temporary[272];
    char current_revision[48];
    struct stat st;
    FILE *file;
    if (!safe_skill_name(name) || !content || strlen(content) > 32768) {
        cJSON_Delete(root); free(body);
        send_envelope(fd, 400, request, NULL, "INVALID_SKILL",
                      "skill name or content is invalid", NULL, false);
        return true;
    }
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, name);
    if (stat(path, &st) == 0 && expected_revision) {
        skill_revision(&st, current_revision, sizeof(current_revision));
        if (strcmp(expected_revision, current_revision) != 0) {
            cJSON_Delete(root); free(body);
            send_envelope(fd, 409, request, NULL, "SKILL_CONFLICT",
                          "skill changed on another client; reload before saving",
                          NULL, false);
            return true;
        }
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "w");
    if (!file || fwrite(content, 1, strlen(content), file) != strlen(content)) {
        if (file) fclose(file);
        unlink(temporary);
        cJSON_Delete(root); free(body);
        send_envelope(fd, 500, request, NULL, "SKILL_WRITE_FAILED",
                      "skill could not be saved", NULL, false);
        return true;
    }
    if (fclose(file) != 0 || rename(temporary, path) != 0) {
        unlink(temporary);
        cJSON_Delete(root); free(body);
        send_envelope(fd, 500, request, NULL, "SKILL_WRITE_FAILED",
                      "skill could not be committed", NULL, false);
        return true;
    }
    cJSON_Delete(root); free(body);
    skill_loader_refresh();
    audit_event("skill.write", name, "success");
    send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    return true;
}

static void advanced_add_config_value(cJSON *data, const char *json_key,
                                      const char *config_key)
{
    char value[256] = { 0 };

    (void)claw_config_get(config_key, value, sizeof(value));
    cJSON_AddStringToObject(data, json_key, value);
}

static bool advanced_configured(const char *config_key)
{
    char value[256] = { 0 };

    return claw_config_get(config_key, value, sizeof(value)) == 0 &&
           value[0] != '\0';
}

static bool handle_advanced_get(int fd, const char *request)
{
    cJSON *data = cJSON_CreateObject();

    advanced_add_config_value(data, "model", "model");
    advanced_add_config_value(data, "llm_host", "llm_host");
    advanced_add_config_value(data, "volc_speaker", "volc_speaker");
    advanced_add_config_value(data, "proxy_host", "proxy_host");
    advanced_add_config_value(data, "proxy_port", "proxy_port");
    advanced_add_config_value(data, "volc_appkey",
                              AGENT_CFG_KEY_VOLC_APPKEY);
    advanced_add_config_value(data, "volc_cluster",
                              AGENT_CFG_KEY_VOLC_CLUSTER);
    advanced_add_config_value(data, "volc_asr_cluster",
                              AGENT_CFG_KEY_VOLC_ASR_CLUSTER);
    cJSON_AddBoolToObject(data, "api_key_configured",
                          advanced_configured("api_key"));
    cJSON_AddBoolToObject(data, "volc_api_key_configured",
                          advanced_configured(AGENT_CFG_KEY_VOLC_API_KEY));
    cJSON_AddBoolToObject(data, "volc_token_configured",
                          advanced_configured(AGENT_CFG_KEY_VOLC_TOKEN));
    send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
    return true;
}

typedef struct {
    const char *json_key;
    const char *config_key;
} advanced_setting_t;

static bool advanced_primary_llm_ready(void)
{
    static const char *keys[] = {
        AGENT_CFG_KEY_LLM_HOST,
        AGENT_CFG_KEY_API_KEY,
        AGENT_CFG_KEY_MODEL,
    };
    char value[256];

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        memset(value, 0, sizeof(value));
        if (claw_config_get(keys[i], value, sizeof(value)) != OK || !value[0]) {
            return false;
        }
    }
    return true;
}

static bool handle_advanced_put(int fd, const char *request, int request_len)
{
    static const advanced_setting_t allowed[] = {
        { "model", "model" },
        { "llm_host", "llm_host" },
        { "api_key", "api_key" },
        { "volc_speaker", "volc_speaker" },
        { "volc_api_key", AGENT_CFG_KEY_VOLC_API_KEY },
        { "volc_appkey", AGENT_CFG_KEY_VOLC_APPKEY },
        { "volc_token", AGENT_CFG_KEY_VOLC_TOKEN },
        { "volc_cluster", AGENT_CFG_KEY_VOLC_CLUSTER },
        { "volc_asr_cluster", AGENT_CFG_KEY_VOLC_ASR_CLUSTER },
        { "proxy_host", "proxy_host" },
        { "proxy_port", "proxy_port" },
    };
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    bool saved = true;
    bool primary_llm_changed = false;
    size_t persisted = 0;
    size_t i;

    if (!root) {
        free(body);
        send_envelope(fd, 400, request, NULL, "INVALID_JSON",
                      "valid JSON is required", NULL, false);
        return true;
    }
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        cJSON *value = cJSON_GetObjectItem(root, allowed[i].json_key);
        char current[256] = { 0 };

        if (!cJSON_IsString(value) || !value->valuestring[0])
            continue;
        if (claw_config_get(allowed[i].config_key, current,
                            sizeof(current)) == OK &&
            strcmp(current, value->valuestring) == 0)
            continue;

        /* File-backed KVDB creates one NAND file per persistent property.
         * Avoid rewriting unchanged fields and leave a short settling gap
         * between real writes.  The R528 NAND stack can otherwise wedge when
         * the browser submits model, host and credentials in one request. */
        if (persisted > 0)
            usleep(ADVANCED_KVDB_SETTLE_US);
        if (claw_config_set(allowed[i].config_key, value->valuestring) != OK)
            saved = false;
        else {
            persisted++;
            if (strcmp(allowed[i].config_key, AGENT_CFG_KEY_LLM_HOST) == 0 ||
                strcmp(allowed[i].config_key, AGENT_CFG_KEY_API_KEY) == 0 ||
                strcmp(allowed[i].config_key, AGENT_CFG_KEY_MODEL) == 0) {
                primary_llm_changed = true;
            }
        }
    }
    /* The response causes the SPA to immediately re-read status, history and
     * settings.  Keep those reads out of the last asynchronous NAND commit
     * window, including the common single-field update case. */
    if (persisted > 0)
        usleep(ADVANCED_KVDB_SETTLE_US);
    if (primary_llm_changed && advanced_primary_llm_ready() &&
        llm_router_sync_primary_from_legacy() != OK) {
        saved = false;
    }
    cJSON_Delete(root);
    if (body) {
        memset(body, 0, strlen(body));
        free(body);
    }
    if (!saved) {
        audit_event("settings.advanced", "masked", "failed");
        send_envelope(fd, 503, request, NULL, "SETTINGS_NOT_SAVED",
                      "one or more settings could not be saved", NULL, false);
        return true;
    }
    audit_event("settings.advanced", "masked", "success");
    send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    return true;
}

static bool handle_device_put(int fd, const char *request, int request_len)
{
    char *body = NULL;
    cJSON *root = parse_body(fd, request, request_len, &body);
    const char *name = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "name")) : NULL;
    cJSON *volume = root ? cJSON_GetObjectItem(root, "volume") : NULL;
    char input[64];
    char output[256];
    if (!name || strlen(name) > 32 || !cJSON_IsNumber(volume) ||
        volume->valueint < 0 || volume->valueint > 100) {
        cJSON_Delete(root); free(body);
        send_envelope(fd, 400, request, NULL, "INVALID_DEVICE_SETTINGS",
                      "device name or volume is invalid", NULL, false);
        return true;
    }
    claw_config_set("device.name", name);
    snprintf(input, sizeof(input), "{\"volume\":%lld}",
             (long long)volume->valueint);
    tool_music_set_volume_execute(input, output, sizeof(output));
    cJSON_Delete(root); free(body);
    audit_event("settings.device", name, "success");
    send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
    return true;
}

static void *maintenance_worker(void *arg)
{
    char *kind = arg;
    sleep(2);
    free(kind);
    /* ai_agent is started from rcS rather than a persistent process supervisor.
     * A controlled board reset is therefore the only restart path that cannot
     * leave the management gateway offline.  Both maintenance actions preserve
     * data and return before this delayed reset runs. */
    boardctl(BOARDIOC_RESET, BOARDIOC_SOFTRESETCAUSE_USER_REBOOT);
    return NULL;
}

static bool handle_maintenance(int fd, const char *request, int request_len,
                               const char *kind)
{
    char *body = NULL;
    cJSON *root;
    const char *confirm;
    const char *password;
    operation_t *operation;
    pthread_t thread;
    pthread_attr_t attr;
    if (strcmp(kind, "clear-cache") == 0) {
        audit_event("maintenance.clear_cache", "temporary", "success");
        send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
        return true;
    }
    root = parse_body(fd, request, request_len, &body);
    confirm = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "confirm")) : NULL;
    password = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "password")) : NULL;
    if (!confirm || strcmp(confirm, strcmp(kind, "reboot") == 0 ? "REBOOT" : "RESTART") != 0 ||
        !admin_auth_verify_password(password)) {
        cJSON_Delete(root); if (body) { memset(body, 0, strlen(body)); free(body); }
        send_envelope(fd, 403, request, NULL, "REAUTH_FAILED",
                      "password and confirmation are required", NULL, false);
        return true;
    }
    cJSON_Delete(root); if (body) { memset(body, 0, strlen(body)); free(body); }
    operation = operation_create(kind);
    operation_update(operation->id, OP_RUNNING, "scheduled in 2 seconds");
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2048);
    if (pthread_create(&thread, &attr, maintenance_worker, strdup(kind)) != 0) {
        pthread_attr_destroy(&attr);
        operation_update(operation->id, OP_FAILED, "worker unavailable");
        send_envelope(fd, 500, request, NULL, "WORKER_FAILED",
                      "maintenance operation could not start", NULL, false);
        return true;
    }
    pthread_attr_destroy(&attr);
    audit_event(strcmp(kind, "reboot") == 0 ? "maintenance.reboot" :
                "maintenance.restart", "system", "accepted");
    send_envelope(fd, 202, request, operation_json(operation), NULL, NULL, NULL, false);
    return true;
}

int admin_api_init(void)
{
    return admin_auth_init();
}

bool admin_api_try_handle(int fd, const char *request, int request_len)
{
    char method[8] = { 0 };
    char path[160] = { 0 };
    admin_session_view_t session;
    bool write;
    if (!request || sscanf(request, "%7s %159s", method, path) != 2 ||
        strncmp(path, "/api/v2/", 8) != 0)
        return false;
    char *query = strchr(path, '?'); if (query) *query++ = '\0';
    write = strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0;

    if (write && !admin_api_origin_valid(request)) {
        send_envelope(fd, 403, request, NULL, "ORIGIN_INVALID",
                      "same-origin request required", NULL, false);
        return true;
    }
    if (write && !write_request_is_fresh(request)) {
        send_envelope(fd, 409, request, NULL, "REQUEST_REPLAYED",
                      "a fresh request ID is required", NULL, false);
        return true;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/auth/state") == 0)
        return handle_auth_state(fd, request);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/auth/setup") == 0)
        return handle_auth_submit(fd, request, request_len, true);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/auth/pair") == 0)
        return handle_auth_submit(fd, request, request_len, true);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/auth/login") == 0)
        return handle_auth_submit(fd, request, request_len, false);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/auth/logout") == 0) {
        if (!require_admin(fd, request, true, &session)) return true;
        admin_auth_logout(request);
        send_envelope(fd, 200, request, cJSON_CreateObject(), NULL, NULL, NULL, true);
        return true;
    }

    if (!require_admin(fd, request, write, &session)) return true;
    const char *chat_requests = "/api/v2/agent/requests";
    size_t chat_requests_len = strlen(chat_requests);
    if (!strcmp(method, "GET") && !strncmp(path, chat_requests, chat_requests_len) &&
        (!path[chat_requests_len] || path[chat_requests_len] == '/')) {
        const char *id = path[chat_requests_len] ? path + chat_requests_len + 1 : NULL;
        /* One administrator identity owns the fixed portal conversation.
         * Session credentials rotate; they must not hide persisted results. */
        cJSON *data = portal_chat_get("administrator", id);
        if (!data) send_envelope(fd, 404, request, NULL, "CHAT_REQUEST_UNAVAILABLE",
            "请求不存在、无权限或请求账本需要恢复；请勿自动重新执行", NULL, false);
        else send_envelope(fd, 200, request, data, NULL, NULL, NULL, false);
        return true;
    }
    if (!strcmp(method, "POST") && !strcmp(path, chat_requests)) {
        char *body = NULL;
        cJSON *root = parse_body(fd, request, request_len, &body), *data = NULL;
        const char *id = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "request_id")) : NULL;
        const char *content = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "content")) : NULL;
        int ret = root ? portal_chat_submit("administrator", session.token, id, content, &data) : -EINVAL;
        cJSON_Delete(root); free(body);
        if (ret) send_envelope(fd, ret == -EACCES ? 403 : ret == -EINVAL ? 400 : ret == -ESTALE || ret == -EEXIST || ret == -EBUSY ? 409 : 503,
            request, NULL, ret == -ESTALE ? "CHAT_REQUEST_RETIRED" : ret == -EEXIST ? "CHAT_REQUEST_CONFLICT" : "CHAT_REQUEST_REJECTED",
            "请求未接受；旧请求不得换编号自动重试，先核对结果或存储状态", NULL, false);
        else send_envelope(fd, 202, request, data, NULL, NULL, NULL, false);
        return true;
    }
    const char *operations = "/api/v2/agent/operations";
    size_t operations_len = strlen(operations);
    if (!strncmp(path, operations, operations_len) &&
        (!path[operations_len] || path[operations_len] == '/'))
        return handle_portal_operations(fd, request, request_len, method,
            path + operations_len + (path[operations_len] == '/' ? 1 : 0), &session);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/status") == 0)
        return handle_status(fd, request);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/labtwin/snapshot") == 0)
        return handle_snapshot(fd, request);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/data/experiments") == 0)
        return handle_experiment_history(fd, request, query);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/data/experiments") == 0)
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_experiment_create_json,
                                           "experiment.create");
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/agent/history") == 0)
        return handle_portal_history(fd, request);
    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/v2/agent/history") == 0)
        return handle_portal_history_clear(fd, request);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/recordings") == 0)
        return handle_recordings_get(fd, request);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/recordings") == 0)
        return handle_recordings_start(fd, request);
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/v2/recordings/", 19) == 0 &&
        path_ends_with(path, "/stop")) {
        char id[16];
        size_t length = strlen(path + 19) - strlen("/stop");
        if (length >= sizeof(id)) length = 0;
        memcpy(id, path + 19, length); id[length] = '\0';
        return handle_recordings_stop(fd, request, id);
    }
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/v2/recordings/", 19) == 0 &&
        path_ends_with(path, "/audio")) {
        char id[16];
        size_t length = strlen(path + 19) - strlen("/audio");
        if (length >= sizeof(id)) length = 0;
        memcpy(id, path + 19, length); id[length] = '\0';
        return send_recording_audio(fd, request, id);
    }
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/v2/recordings/", 19) == 0 &&
        !strchr(path + 19, '/'))
        return handle_recordings_delete(fd, request, path + 19);
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_detail(fd, request, path + 25);
    if (strcmp(method, "PUT") == 0 && strncmp(path, "/api/v2/data/experiments/", 25) == 0 &&
        !strchr(path + 25, '/'))
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_experiment_update_json,
                                           "experiment.update");
    if (strcmp(method, "POST") == 0 && strstr(path, "/transitions") &&
        strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_experiment_transition_json,
                                           "experiment.transition");
    if (strcmp(method, "POST") == 0 && strstr(path, "/observations") &&
        strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_log_add_json,
                                           "experiment.observe");
    if (strcmp(method, "POST") == 0 && strstr(path, "/timers") &&
        strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_timer_start_json,
                                           "experiment.timer.start");
    if (strcmp(method, "DELETE") == 0 && strstr(path, "/timers/") &&
        strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_operation(fd, request, request_len,
                                           labtwin_timer_cancel_json,
                                           "experiment.timer.cancel");
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/network/status") == 0) {
        send_envelope(fd, 200, request, wifi_json(true), NULL, NULL, NULL, false);
        return true;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/network/scan") == 0) {
        if (device_wifi_scan() != OK)
            send_envelope(fd, 409, request, NULL, "SCAN_BUSY", "wifi scan unavailable", NULL, false);
        else
            send_envelope(fd, 202, request, cJSON_CreateObject(), NULL, NULL, NULL, false);
        return true;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/network/connect") == 0)
        return handle_network_connect(fd, request, request_len);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/network/disconnect") == 0) {
        device_wifi_disconnect(); audit_event("network.disconnect", "wlan0", "accepted");
        send_envelope(fd, 202, request, cJSON_CreateObject(), NULL, NULL, NULL, false); return true;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/network/forget") == 0) {
        device_wifi_forget(); audit_event("network.forget", "wlan0", "accepted");
        send_envelope(fd, 202, request, cJSON_CreateObject(), NULL, NULL, NULL, false); return true;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/bluetooth/status") == 0) {
        send_envelope(fd, 200, request, ble_json(), NULL, NULL, NULL, false);
        return true;
    }
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/v2/bluetooth/", 18) == 0)
        return handle_ble_action(fd, request, request_len, path + 18);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/settings/environment") == 0)
        return handle_environment_get(fd, request);
    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/v2/settings/environment") == 0)
        return handle_environment_put(fd, request, request_len);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/settings/environment/reset") == 0) {
        char output[1024];
        if (labtwin_environment_rule_reset_json(output, sizeof(output)) != 0)
            send_envelope(fd, 500, request, NULL, "RESET_FAILED", "rules were not reset", NULL, false);
        else { audit_event("settings.environment.reset", "rules", "success"); handle_environment_get(fd, request); }
        return true;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/settings/advanced") == 0)
        return handle_advanced_get(fd, request);
    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/v2/settings/advanced") == 0)
        return handle_advanced_put(fd, request, request_len);
    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/v2/settings/device") == 0)
        return handle_device_put(fd, request, request_len);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/skills") == 0)
        return handle_skills_get_v2(fd, request);
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/v2/skills/", 15) == 0)
        return handle_skill_get_v2(fd, request, path + 15);
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/v2/skills") == 0)
        return handle_skills_post_v2(fd, request, request_len);
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/v2/skills/", 15) == 0)
        return handle_skill_delete_v2(fd, request, path + 15);
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/v2/data/experiments/", 25) == 0)
        return handle_experiment_delete(fd, request, request_len, path + 25);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/v2/logs") == 0) {
        cJSON *data = cJSON_CreateObject(); cJSON_AddItemToObject(data, "logs", agent_logbuf_dump());
        send_envelope(fd, 200, request, data, NULL, NULL, NULL, false); return true;
    }
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/v2/maintenance/", 20) == 0)
        return handle_maintenance(fd, request, request_len, path + 20);
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/v2/operations/", 19) == 0) {
        unsigned int id = (unsigned int)strtoul(path + 19, NULL, 10); int i;
        pthread_mutex_lock(&g_api_lock);
        for (i = 0; i < OPERATION_MAX; i++) if (g_operations[i].used && g_operations[i].id == id) break;
        if (i < OPERATION_MAX) { cJSON *data = operation_json(&g_operations[i]); pthread_mutex_unlock(&g_api_lock); send_envelope(fd, 200, request, data, NULL, NULL, NULL, false); }
        else { pthread_mutex_unlock(&g_api_lock); send_envelope(fd, 404, request, NULL, "NOT_FOUND", "operation not found", NULL, false); }
        return true;
    }
    send_envelope(fd, 404, request, NULL, "NOT_FOUND", "API route not found", NULL, false);
    return true;
}
