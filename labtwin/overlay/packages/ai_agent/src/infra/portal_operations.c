#include "infra/portal_operations.h"
#include "infra/admin_auth.h"
#include "infra/admin_api.h"
#include "labtwin/labtwin.h"
#include "tools/tool_registry.h"
#include "tools/tool_guard.h"
#include "mbedtls/sha256.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef PORTAL_LEDGER_PATH
#define PORTAL_LEDGER_PATH "/data/labtwin/portal-operations.json"
#endif
#define PENDING_MAX 8
#define LEDGER_MAX 64
enum op_state { OP_PENDING, OP_EXECUTING, OP_APPLIED, OP_FAILED, OP_CANCELLED, OP_EXPIRED, OP_UNCERTAIN };
static const char *const states[] = {"PENDING", "EXECUTING", "APPLIED", "FAILED", "CANCELLED", "EXPIRED", "UNCERTAIN"};
typedef struct {
    char id[33], owner[65], token_hash[65], experiment_id[32], action[16];
    uint64_t sequence;
    enum op_state state;
    bool used;
    char result[128];
} ledger_t;
typedef struct {
    char id[33], token[65];
    uint64_t deadline;
    bool used;
} pending_t;
static pthread_mutex_t g_ops_lock = PTHREAD_MUTEX_INITIALIZER;
static ledger_t g_ledger[LEDGER_MAX];
static pending_t g_pending[PENDING_MAX];
static unsigned int g_cursor;
static bool g_loaded;
extern void ws_server_pending_operation(const char *, const char *) __attribute__((weak));

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static bool valid_experiment_id(const char *id)
{
    if (!id || !id[0] || strlen(id) >= 32) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++)
        if (!isalnum(*p) && *p != '-' && *p != '_') return false;
    return true;
}
static void hash(const char *text, char out[65])
{
    unsigned char bytes[32];
    mbedtls_sha256((const unsigned char *)text, strlen(text), bytes, 0);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", bytes[i]);
}
static int random_hex(char *out, size_t bytes)
{
    unsigned char random[32];
    size_t got = 0;
    /* arc4random_buf on this NuttX revision can fall back to clock hashes.
     * Tokens must instead fail closed when the hardware RNG is unavailable. */
    while (got < bytes) {
        ssize_t n = getrandom(random + got, bytes - got, GRND_NONBLOCK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { memset(random, 0, sizeof(random)); return -EIO; }
        got += (size_t)n;
    }
    for (size_t i = 0; i < bytes; i++) snprintf(out + i * 2, 3, "%02x", random[i]);
    memset(random, 0, sizeof(random));
    return 0;
}
static pending_t *pending_for(const char *id)
{
    for (int i = 0; i < PENDING_MAX; i++)
        if (g_pending[i].used && !strcmp(g_pending[i].id, id)) return &g_pending[i];
    return NULL;
}
static cJSON *view(const ledger_t *op, bool persisted)
{
    cJSON *item = cJSON_CreateObject();
    pending_t *pending = pending_for(op->id);
    cJSON_AddStringToObject(item, "id", op->id);
    cJSON_AddStringToObject(item, "experiment_id", op->experiment_id);
    cJSON_AddStringToObject(item, "action", op->action);
    cJSON_AddStringToObject(item, "state", states[op->state]);
    cJSON_AddStringToObject(item, "result", op->result);
    cJSON_AddNumberToObject(item, "if_event_seq", (double)op->sequence);
    if (persisted) {
        cJSON_AddStringToObject(item, "owner_hash", op->owner);
        cJSON_AddStringToObject(item, "token_hash", op->token_hash);
    } else {
        cJSON_AddBoolToObject(item, "password_required", !admin_auth_private_open());
        if (pending && op->state == OP_PENDING) {
            uint64_t now = now_ms();
            cJSON_AddStringToObject(item, "token", pending->token);
            cJSON_AddNumberToObject(item, "remaining_seconds",
                pending->deadline > now ? (double)((pending->deadline - now + 999) / 1000) : 0);
        }
    }
    return item;
}
static int persist(void)
{
    char temp[256];
    cJSON *array = cJSON_CreateArray();
    char *json;
    int fd, ret = 0;
    size_t length, written = 0;
    for (int i = 0; i < LEDGER_MAX; i++)
        if (g_ledger[i].used && g_ledger[i].state != OP_PENDING)
            cJSON_AddItemToArray(array, view(&g_ledger[i], true));
    json = cJSON_PrintUnformatted(array); cJSON_Delete(array);
    if (!json) return -ENOMEM;
    snprintf(temp, sizeof(temp), "%s.tmp", PORTAL_LEDGER_PATH);
    fd = open(temp, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    length = strlen(json);
    if (fd < 0) ret = -errno;
    while (!ret && written < length) {
        ssize_t n = write(fd, json + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ret = -EIO;
        else written += (size_t)n;
    }
    if (fd >= 0) {
        if (!ret && fsync(fd) < 0) ret = -errno;
        if (close(fd) < 0) ret = -errno;
    }
    if (!ret && rename(temp, PORTAL_LEDGER_PATH) < 0) ret = -errno;
    free(json);
    return ret;
}
static int load(void)
{
    FILE *file;
    long size;
    char *json;
    cJSON *array, *item;
    int count = 0;
    if (g_loaded) return 0;
    file = fopen(PORTAL_LEDGER_PATH, "r");
    if (!file) {
        if (errno != ENOENT) return -EIO;
        g_loaded = true; return 0;
    }
    fseek(file, 0, SEEK_END); size = ftell(file); rewind(file);
    if (size <= 0 || size > 65536) { fclose(file); return -EIO; }
    json = malloc((size_t)size + 1);
    if (!json) { fclose(file); return -ENOMEM; }
    size_t got = fread(json, 1, (size_t)size, file);
    fclose(file); json[got] = 0;
    array = cJSON_Parse(json); free(json);
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > LEDGER_MAX) {
        cJSON_Delete(array); return -EIO;
    }
    memset(g_ledger, 0, sizeof(g_ledger));
    cJSON_ArrayForEach(item, array) {
        ledger_t *op = &g_ledger[count++];
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *owner = cJSON_GetStringValue(cJSON_GetObjectItem(item, "owner_hash"));
        const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(item, "token_hash"));
        const char *experiment = cJSON_GetStringValue(cJSON_GetObjectItem(item, "experiment_id"));
        const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(item, "action"));
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(item, "state"));
        const char *result = cJSON_GetStringValue(cJSON_GetObjectItem(item, "result"));
        if (!id || strlen(id) != 32 || !owner || strlen(owner) != 64 || !token || strlen(token) != 64 ||
            !experiment || strlen(experiment) >= sizeof(op->experiment_id) || !action || strlen(action) >= sizeof(op->action)) {
            memset(g_ledger, 0, sizeof(g_ledger)); cJSON_Delete(array); return -EIO;
        }
        snprintf(op->id, sizeof(op->id), "%s", id);
        snprintf(op->owner, sizeof(op->owner), "%s", owner);
        snprintf(op->token_hash, sizeof(op->token_hash), "%s", token);
        snprintf(op->experiment_id, sizeof(op->experiment_id), "%s", experiment);
        snprintf(op->action, sizeof(op->action), "%s", action);
        snprintf(op->result, sizeof(op->result), "%s", result ? result : "");
        op->sequence = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "if_event_seq"));
        op->state = OP_UNCERTAIN;
        for (int i = OP_APPLIED; i <= OP_UNCERTAIN; i++)
            if (state && !strcmp(state, states[i])) op->state = (enum op_state)i;
        op->used = true;
    }
    cJSON_Delete(array); g_loaded = true;
    return 0;
}
static void expire(void)
{
    for (int i = 0; i < LEDGER_MAX; i++) {
        ledger_t *op = &g_ledger[i];
        pending_t *pending = op->used ? pending_for(op->id) : NULL;
        if (op->used && op->state == OP_PENDING && (!pending || pending->deadline <= now_ms())) {
            op->state = OP_EXPIRED;
            if (pending) memset(pending, 0, sizeof(*pending));
        }
    }
}
static int prepare(const char *owner, const char *id, const char *action, char *out, size_t size)
{
    char request[96], owner_hash[65];
    char *snapshot = malloc(16384);
    cJSON *root, *data;
    uint64_t sequence;
    int slot = -1, ledger_slot = -1;
    if (!snapshot) return -ENOMEM;
    snprintf(request, sizeof(request), "{\"experiment_id\":\"%s\"}", id);
    if (labtwin_experiment_get_json(request, snapshot, 16384)) { free(snapshot); return -ENOENT; }
    root = cJSON_Parse(snapshot); data = root ? cJSON_GetObjectItem(root, "data") : NULL;
    free(snapshot);
    sequence = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(data, "last_event_seq"));
    cJSON_Delete(root);
    if (!sequence) return -EINVAL;
    hash(owner, owner_hash);
    pthread_mutex_lock(&g_ops_lock);
    if (load()) { pthread_mutex_unlock(&g_ops_lock); return -EIO; }
    expire();
    for (int i = 0; i < PENDING_MAX; i++) if (!g_pending[i].used) { slot = i; break; }
    for (int i = 0; i < LEDGER_MAX; i++) {
        int index = (int)((g_cursor + (unsigned int)i) % LEDGER_MAX);
        ledger_t *op = &g_ledger[index];
        if (!op->used || (op->state != OP_PENDING && op->state != OP_EXECUTING && op->state != OP_UNCERTAIN)) {
            ledger_slot = index; break;
        }
    }
    if (slot < 0 || ledger_slot < 0) { pthread_mutex_unlock(&g_ops_lock); return -ENOSPC; }
    ledger_t *op = &g_ledger[ledger_slot];
    pending_t *pending = &g_pending[slot];
    memset(op, 0, sizeof(*op)); memset(pending, 0, sizeof(*pending));
    if (random_hex(op->id, 16) || random_hex(pending->token, 32)) {
        memset(op, 0, sizeof(*op)); memset(pending, 0, sizeof(*pending));
        pthread_mutex_unlock(&g_ops_lock); return -EIO;
    }
    snprintf(pending->id, sizeof(pending->id), "%s", op->id);
    snprintf(op->owner, sizeof(op->owner), "%s", owner_hash);
    hash(pending->token, op->token_hash);
    snprintf(op->experiment_id, sizeof(op->experiment_id), "%s", id);
    snprintf(op->action, sizeof(op->action), "%s", action);
    op->sequence = sequence; op->used = true; op->state = OP_PENDING;
    pending->used = true; pending->deadline = now_ms() + 120000;
    g_cursor = (unsigned int)ledger_slot + 1;
    /* The model receives no confirmation token; only the admin REST view does. */
    snprintf(out, size, "{\"ok\":true,\"code\":\"CONFIRMATION_REQUIRED\",\"operation_id\":\"%s\",\"message\":\"等待网页确认；尚未执行\"}", op->id);
    char operation_id[33];
    snprintf(operation_id, sizeof(operation_id), "%s", op->id);
    pthread_mutex_unlock(&g_ops_lock);
    if (ws_server_pending_operation) ws_server_pending_operation(owner, operation_id);
    return 0;
}
cJSON *portal_operations_get(const char *owner, const char *id)
{
    char owner_hash[65];
    cJSON *result = id ? NULL : cJSON_CreateObject();
    cJSON *array = id ? NULL : cJSON_AddArrayToObject(result, "operations");
    hash(owner, owner_hash);
    pthread_mutex_lock(&g_ops_lock);
    if (load()) { pthread_mutex_unlock(&g_ops_lock); cJSON_Delete(result); return NULL; }
    expire();
    for (int i = 0; i < LEDGER_MAX; i++) {
        ledger_t *op = &g_ledger[i];
        if (!op->used || strcmp(op->owner, owner_hash) || (id && strcmp(id, op->id))) continue;
        if (id) result = view(op, false);
        else cJSON_AddItemToArray(array, view(op, false));
    }
    pthread_mutex_unlock(&g_ops_lock);
    return result;
}
int portal_operation_resolve(const char *owner, const char *id, cJSON *body, bool cancel, cJSON **result)
{
    char owner_hash[65], token_hash[65], input[192], output[1024];
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(body, "token"));
    ledger_t *op = NULL;
    int ret = 0;
    *result = NULL;
    if (!token || strlen(token) != 64) return -EACCES;
    hash(owner, owner_hash); hash(token, token_hash);
    pthread_mutex_lock(&g_ops_lock);
    if (load()) { ret = -EIO; goto done; }
    expire();
    for (int i = 0; i < LEDGER_MAX; i++)
        if (g_ledger[i].used && !strcmp(g_ledger[i].id, id)) { op = &g_ledger[i]; break; }
    if (!op || strcmp(op->owner, owner_hash) || strcmp(op->token_hash, token_hash)) { ret = -EACCES; goto done; }
    if (op->state != OP_PENDING) { *result = view(op, false); goto done; }
    if (!cancel && !strcmp(op->action, "delete")) {
        const char *confirm = cJSON_GetStringValue(cJSON_GetObjectItem(body, "confirm_experiment_id"));
        const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(body, "password"));
        if (!confirm || strcmp(confirm, op->experiment_id) ||
            (!admin_auth_private_open() && !admin_auth_verify_password(password))) { ret = -EACCES; goto done; }
    }
    pending_t *pending = pending_for(op->id);
    uint64_t deadline = pending ? pending->deadline : 0;
    op->state = cancel ? OP_CANCELLED : OP_EXECUTING;
    if (persist()) { op->state = OP_UNCERTAIN; ret = -EIO; goto done; }
    if (pending) memset(pending, 0, sizeof(*pending));
    if (!cancel) {
        if (now_ms() >= deadline) {
            op->state = OP_EXPIRED;
            if (persist()) op->state = OP_UNCERTAIN;
            *result = view(op, false);
            goto done;
        }
        if (tool_guard_check(!strcmp(op->action, "delete") ? "experiment_delete" : "experiment_transition", "{}", 2) != GUARD_ALLOW) {
            op->state = OP_FAILED;
            snprintf(op->result, sizeof(op->result), "工具已禁用或受限，未执行");
            if (persist()) op->state = OP_UNCERTAIN;
            *result = view(op, false);
            goto done;
        }
        if (!strcmp(op->action, "delete")) ret = admin_delete_experiment(op->experiment_id, op->sequence, "portal-agent");
        else {
            snprintf(input, sizeof(input), "{\"experiment_id\":\"%s\",\"action\":\"%s\",\"if_event_seq\":%llu}",
                     op->experiment_id, op->action, (unsigned long long)op->sequence);
            ret = labtwin_experiment_transition_json(input, output, sizeof(output), "portal-agent");
        }
        op->state = ret == -EUCLEAN ? OP_UNCERTAIN : ret ? OP_FAILED : OP_APPLIED;
        snprintf(op->result, sizeof(op->result), "%s", ret == -EUCLEAN ? "结果不确定；任务已隔离，请检查记录，勿自动重试" : ret ? "未执行或失败；请检查任务状态后重新提议" : "操作已提交");
        if (persist()) { op->state = OP_UNCERTAIN; ret = -EIO; }
    }
    *result = view(op, false);
    syslog(LOG_INFO, "[portal-agent-audit] action=%s target=%s state=%s\n", op->action, op->experiment_id, states[op->state]);
    ret = 0; /* terminal outcome is in state; never invite automatic execution retry */
done:
    pthread_mutex_unlock(&g_ops_lock);
    return ret;
}
int portal_tool_execute(const char *name, const char *input, const char *owner, char *out, size_t size)
{
    int ret = -EINVAL;
    if (!admin_auth_token_valid(owner)) {
        snprintf(out, size, "{\"ok\":false,\"code\":\"AUTH_REQUIRED\"}"); return -EACCES;
    }
    static const char *const guarded[] = {"experiment_transition", "experiment_delete", "experiment_create",
        "experiment_update", "timer_start", "timer_cancel", "lab_log_add", "controller_command"};
    for (size_t i = 0; i < sizeof(guarded) / sizeof(guarded[0]); i++) {
        if (!strcmp(name, guarded[i])) {
            if (tool_guard_check(name, input, input ? strlen(input) : 0) != GUARD_ALLOW) {
                snprintf(out, size, "{\"ok\":false,\"code\":\"TOOL_GUARD_REJECTED\"}"); return -EACCES;
            }
            tool_guard_record_call(name);
            break;
        }
    }
    if (!strcmp(name, "experiment_transition") || !strcmp(name, "experiment_delete")) {
        cJSON *root = cJSON_Parse(input ? input : "{}");
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "experiment_id"));
        const char *action = !strcmp(name, "experiment_delete") ? "delete" :
            cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
        if (valid_experiment_id(id) && action &&
            (!strcmp(action, "complete") || !strcmp(action, "cancel") || !strcmp(action, "delete")))
            ret = prepare(owner, id, action, out, size);
        else if (!strcmp(name, "experiment_transition"))
            ret = labtwin_experiment_transition_json(input, out, size, "portal-agent");
        cJSON_Delete(root);
        if (ret) snprintf(out, size, "{\"ok\":false,\"code\":\"OPERATION_REJECTED\",\"error\":%d}", ret);
        return ret;
    }
    if (!strcmp(name, "experiment_create")) return labtwin_experiment_create_json(input, out, size, "portal-agent");
    if (!strcmp(name, "experiment_update")) return labtwin_experiment_update_json(input, out, size, "portal-agent");
    if (!strcmp(name, "timer_start")) return labtwin_timer_start_json(input, out, size, "portal-agent");
    if (!strcmp(name, "timer_cancel")) return labtwin_timer_cancel_json(input, out, size, "portal-agent");
    if (!strcmp(name, "lab_log_add")) return labtwin_log_add_json(input, out, size, "portal-agent");
    /* Controller projection can indirectly complete/cancel an experiment. */
    if (!strcmp(name, "controller_command")) {
        snprintf(out, size, "{\"ok\":false,\"code\":\"USE_CONFIRMED_EXPERIMENT_TOOLS\"}"); return -EACCES;
    }
    return tool_registry_execute(name, input, out, size);
}
