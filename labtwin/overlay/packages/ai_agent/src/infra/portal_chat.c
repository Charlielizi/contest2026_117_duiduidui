/* Copyright 2026. Licensed under the Apache License, Version 2.0. */
#include "infra/portal_chat.h"
#include "core/message_bus.h"
#include "core/session_mgr.h"
#include "mbedtls/sha256.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PORTAL_CHAT_PATH
#define PORTAL_CHAT_PATH "/data/ai_agent/portal-chat.json"
#endif
#define CHAT_SLOTS 8
#define CHAT_DEADLINE_MS 120000
#define CHAT_REPLY_MAX 4096
enum chat_state { CHAT_QUEUED, CHAT_RUNNING, CHAT_SUCCEEDED, CHAT_FAILED, CHAT_UNCERTAIN };
static const char *const states[] = { "QUEUED", "RUNNING", "SUCCEEDED", "FAILED", "UNCERTAIN" };
typedef struct {
    bool used, executing, late;
    char id[25], owner_hash[65], content_hash[65], content[1025];
    char reply[CHAT_REPLY_MAX + 1], error[48];
    uint64_t ordinal, deadline;
    enum chat_state state;
} chat_t;
static pthread_mutex_t g_chat_lock = PTHREAD_MUTEX_INITIALIZER;
static chat_t g_chat[CHAT_SLOTS];
static uint64_t g_ordinal;
static bool g_loaded;
static int g_storage_error;

#ifndef PORTAL_CHAT_NOW
static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
#define PORTAL_CHAT_NOW monotonic_ms
#endif
static void hash(const char *value, char result[65])
{
    unsigned char digest[32];
    mbedtls_sha256((const unsigned char *)value, strlen(value), digest, 0);
    for (int i = 0; i < 32; i++) snprintf(result + i * 2, 3, "%02x", digest[i]);
}
static bool hex_string(const char *value, size_t length)
{
    if (!value || strlen(value) != length) return false;
    for (size_t i = 0; i < length; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}
/* Compact immutable tombstones keep evicted ids from ever being re-executed.
 * Only eight full request/results are cached; no probabilistic rejection of
 * fresh ids and no lifetime bound on the number of conversations. */
static int retired_id(const char *id, bool save)
{
    char dir[256], path[288];
    snprintf(dir, sizeof(dir), "%s.retired", PORTAL_CHAT_PATH);
    snprintf(path, sizeof(path), "%s/%s", dir, id);
    if (!save) {
        struct stat st;
        if (!stat(path, &st)) return 1;
        return errno == ENOENT ? 0 : -errno;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -errno;
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) return errno == EEXIST ? 0 : -errno;
    int ret = write(fd, "retired\n", 8) == 8 ? 0 : -EIO;
    if (!ret && fsync(fd)) ret = -errno;
    if (close(fd) && !ret) ret = -errno;
    return ret;
}
static chat_t *find(const char *id)
{
    for (int i = 0; i < CHAT_SLOTS; i++)
        if (g_chat[i].used && !strcmp(g_chat[i].id, id)) return &g_chat[i];
    return NULL;
}
static cJSON *view(const chat_t *item, bool persisted)
{
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;
    cJSON_AddStringToObject(result, "request_id", item->id);
    cJSON_AddStringToObject(result, "state", states[item->state]);
    cJSON_AddStringToObject(result, "content", item->content);
    cJSON_AddStringToObject(result, "reply", item->reply);
    cJSON_AddStringToObject(result, "error", item->error);
    cJSON_AddBoolToObject(result, "late", item->late);
    cJSON_AddNumberToObject(result, "ordinal", (double)item->ordinal);
    uint64_t now = PORTAL_CHAT_NOW();
    cJSON_AddNumberToObject(result, "remaining_seconds", item->deadline > now ? (double)((item->deadline - now + 999) / 1000) : 0);
    if (persisted) {
        cJSON_AddStringToObject(result, "owner_hash", item->owner_hash);
        cJSON_AddStringToObject(result, "content_hash", item->content_hash);
    }
    if (cJSON_GetArraySize(result) != (persisted ? 10 : 8)) {
        cJSON_Delete(result); return NULL;
    }
    return result;
}
/* The only authoritative reservation point is durable atomic publication. */
static int persist(void)
{
    cJSON *root = cJSON_CreateObject(), *items = cJSON_CreateArray();
    if (!root || !items) { cJSON_Delete(root); cJSON_Delete(items); return -ENOMEM; }
    if (!cJSON_AddItemToObject(root, "requests", items)) {
        cJSON_Delete(root); cJSON_Delete(items); return -ENOMEM;
    }
    for (int i = 0; i < CHAT_SLOTS; i++) if (g_chat[i].used) {
        cJSON *entry = view(&g_chat[i], true);
        if (!entry) { cJSON_Delete(root); return -ENOMEM; }
        if (!cJSON_AddItemToArray(items, entry)) {
            cJSON_Delete(entry); cJSON_Delete(root); return -ENOMEM;
        }
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!json) return -ENOMEM;
    char path[256]; snprintf(path, sizeof(path), "%s.tmp", PORTAL_CHAT_PATH);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600), ret = 0;
    size_t written = 0, length = strlen(json);
    if (fd < 0) ret = -errno;
    while (!ret && written < length) {
        ssize_t n = write(fd, json + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ret = n < 0 ? -errno : -EIO;
        else written += (size_t)n;
    }
    if (fd >= 0) {
        if (!ret && fsync(fd) != 0) ret = -errno;
        if (close(fd) != 0 && !ret) ret = -errno;
    }
    if (!ret && rename(path, PORTAL_CHAT_PATH) != 0) ret = -errno;
#ifdef __NuttX__
    /* YAFFS rename metadata must be synchronized before tools may execute. */
    if (!ret) {
        int saved = open(PORTAL_CHAT_PATH, O_RDONLY);
        if (saved < 0) ret = -errno;
        else { if (syncfs(saved) != 0) ret = -errno; close(saved); }
    }
#endif
    if (ret) unlink(path);
    free(json);
    return ret;
}
static const char *field(cJSON *item, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, name));
}
static void load(void)
{
    if (g_loaded) return;
    g_loaded = true;
    FILE *file = fopen(PORTAL_CHAT_PATH, "rb");
    if (!file) { if (errno != ENOENT) g_storage_error = -errno; return; }
    char *json = NULL; long size = 0;
    if (fseek(file, 0, SEEK_END) == 0) { size = ftell(file); rewind(file); }
    if (size > 0 && size <= 192 * 1024) json = malloc((size_t)size + 1);
    if (!json || fread(json, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); free(json); g_storage_error = -EIO; return;
    }
    fclose(file); json[size] = 0;
    cJSON *root = cJSON_Parse(json); free(json);
    cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "requests") : NULL;
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > CHAT_SLOTS) {
        cJSON_Delete(root); g_storage_error = -EIO; return;
    }
    bool changed = false;
    for (int i = 0; i < cJSON_GetArraySize(items); i++) {
        cJSON *entry = cJSON_GetArrayItem(items, i);
        const char *id = field(entry, "request_id"), *owner = field(entry, "owner_hash");
        const char *content_hash = field(entry, "content_hash"), *content = field(entry, "content");
        const char *reply = field(entry, "reply"), *state = field(entry, "state"), *error = field(entry, "error");
        int index = 0;
        while (index <= CHAT_UNCERTAIN && (!state || strcmp(state, states[index]))) index++;
        cJSON *ordinal = cJSON_GetObjectItemCaseSensitive(entry, "ordinal");
        if (!hex_string(id, 24) || !hex_string(owner, 64) || !hex_string(content_hash, 64) ||
            !content || strlen(content) > 1024 || !reply || strlen(reply) > CHAT_REPLY_MAX ||
            !error || strlen(error) >= 48 || index > CHAT_UNCERTAIN || !cJSON_IsNumber(ordinal) ||
            !isfinite(ordinal->valuedouble) || ordinal->valuedouble < 1 ||
            ordinal->valuedouble > 9007199254740991.0 || floor(ordinal->valuedouble) != ordinal->valuedouble || find(id)) {
            g_storage_error = -EIO; break;
        }
        chat_t *item = &g_chat[i]; item->used = true; item->state = (enum chat_state)index;
        snprintf(item->id, sizeof(item->id), "%s", id);
        snprintf(item->owner_hash, sizeof(item->owner_hash), "%s", owner);
        snprintf(item->content_hash, sizeof(item->content_hash), "%s", content_hash);
        snprintf(item->content, sizeof(item->content), "%s", content);
        snprintf(item->reply, sizeof(item->reply), "%s", reply);
        snprintf(item->error, sizeof(item->error), "%s", error);
        item->ordinal = (uint64_t)ordinal->valuedouble;
        if (item->ordinal > g_ordinal) g_ordinal = item->ordinal;
        if (item->state == CHAT_QUEUED || item->state == CHAT_RUNNING) {
            item->state = CHAT_UNCERTAIN; snprintf(item->error, sizeof(item->error), "RESTART_UNCERTAIN"); changed = true;
        }
    }
    cJSON_Delete(root);
    if (!g_storage_error && changed) g_storage_error = persist();
}
static void expire(void)
{
    if (g_storage_error) return;
    bool changed = false;
    uint64_t now = PORTAL_CHAT_NOW();
    for (int i = 0; i < CHAT_SLOTS; i++) {
        chat_t *item = &g_chat[i];
        if (item->used && (item->state == CHAT_QUEUED || item->state == CHAT_RUNNING) && now >= item->deadline) {
            item->state = CHAT_UNCERTAIN;
            snprintf(item->error, sizeof(item->error), "%s", item->executing ? "TIMEOUT_UNCERTAIN" : "QUEUE_TIMEOUT");
            changed = true;
        }
    }
    if (changed) g_storage_error = persist();
}
int portal_chat_submit(const char *owner, const char *session_token, const char *id, const char *text, cJSON **result)
{
    if (result) *result = NULL;
    if (!owner || !owner[0] || !session_token || !session_token[0] || !hex_string(id, 24) || !text || !text[0] || strlen(text) > 1024) return -EINVAL;
    char owner_hash[65], content_hash[65]; hash(owner, owner_hash); hash(text, content_hash);
    pthread_mutex_lock(&g_chat_lock); load(); expire();
    int ret = g_storage_error;
    chat_t *item = find(id);
    if (!ret && item) {
        if (strcmp(item->owner_hash, owner_hash)) ret = -EACCES;
        else if (strcmp(item->content_hash, content_hash)) ret = -EEXIST;
        else if (result) *result = view(item, false);
        pthread_mutex_unlock(&g_chat_lock); return ret;
    }
    if (!ret) {
        int retired = retired_id(id, false);
        if (retired) ret = retired > 0 ? -ESTALE : retired;
    }
    chat_t *slot = NULL;
    for (int i = 0; !ret && i < CHAT_SLOTS; i++) {
        chat_t *candidate = &g_chat[i];
        if (!candidate->used) { slot = candidate; break; }
        if (!candidate->executing && candidate->state >= CHAT_SUCCEEDED && (!slot || candidate->ordinal < slot->ordinal)) slot = candidate;
    }
    if (!ret && !slot) ret = -EBUSY;
    chat_t *previous = !ret ? malloc(sizeof(*previous)) : NULL;
    if (!ret && !previous) ret = -ENOMEM;
    if (!ret && slot->used) ret = retired_id(slot->id, true);
    if (!ret) {
        *previous = *slot;
        memset(slot, 0, sizeof(*slot)); slot->used = true;
        snprintf(slot->id, sizeof(slot->id), "%s", id);
        snprintf(slot->owner_hash, sizeof(slot->owner_hash), "%s", owner_hash);
        snprintf(slot->content_hash, sizeof(slot->content_hash), "%s", content_hash);
        snprintf(slot->content, sizeof(slot->content), "%s", text);
        slot->ordinal = ++g_ordinal; slot->deadline = PORTAL_CHAT_NOW() + CHAT_DEADLINE_MS;
        ret = persist();
        if (ret) { *slot = *previous; g_storage_error = ret; }
        else {
            agent_msg_t message = {0};
            snprintf(message.channel, sizeof(message.channel), "websocket");
            snprintf(message.chat_id, sizeof(message.chat_id), "portal-admin");
            snprintf(message.portal_session, sizeof(message.portal_session), "%s", session_token);
            snprintf(message.request_id, sizeof(message.request_id), "%s", id);
            message.content = strdup(text);
            if (!message.content || message_bus_push_inbound(&message) != 0) {
                free(message.content); slot->state = CHAT_FAILED;
                snprintf(slot->error, sizeof(slot->error), "QUEUE_UNAVAILABLE");
                g_storage_error = persist();
                ret = g_storage_error;
            }
            if (result) *result = view(slot, false);
        }
    }
    free(previous);
    pthread_mutex_unlock(&g_chat_lock); return ret;
}
cJSON *portal_chat_get(const char *owner, const char *id)
{
    if (!owner || !owner[0]) return NULL;
    char owner_hash[65]; hash(owner, owner_hash);
    pthread_mutex_lock(&g_chat_lock); load(); expire();
    cJSON *result = NULL;
    if (!g_storage_error) {
        if (id) {
            chat_t *item = find(id);
            if (item && !strcmp(item->owner_hash, owner_hash)) result = view(item, false);
        } else {
            result = cJSON_CreateObject(); cJSON *items = cJSON_AddArrayToObject(result, "requests");
            for (int i = 0; i < CHAT_SLOTS; i++) if (g_chat[i].used && g_chat[i].content[0] && !strcmp(g_chat[i].owner_hash, owner_hash)) cJSON_AddItemToArray(items, view(&g_chat[i], false));
        }
    }
    pthread_mutex_unlock(&g_chat_lock); return result;
}
int portal_chat_begin(const char *id)
{
    pthread_mutex_lock(&g_chat_lock); load(); expire();
    chat_t *item = find(id);
    int ret = g_storage_error ? g_storage_error : !item || item->state != CHAT_QUEUED ? -ESTALE : 0;
    if (!ret) {
        item->state = CHAT_RUNNING; item->executing = true;
        ret = persist();
        if (ret) { item->executing = false; g_storage_error = ret; }
    }
    pthread_mutex_unlock(&g_chat_lock); return ret;
}
bool portal_chat_can_execute(const char *id)
{
    if (!id || !id[0]) return true; /* Non-portal transports retain their path. */
    pthread_mutex_lock(&g_chat_lock); load(); expire();
    chat_t *item = find(id);
    bool allowed = !g_storage_error && item && item->state == CHAT_RUNNING && item->executing;
    pthread_mutex_unlock(&g_chat_lock); return allowed;
}
void portal_chat_finish(const char *id, const char *reply, const char *error)
{
    pthread_mutex_lock(&g_chat_lock); load();
    chat_t *item = find(id);
    if (item && !g_storage_error) {
        item->late = PORTAL_CHAT_NOW() >= item->deadline;
        item->executing = false;
        if (reply && reply[0]) {
            item->state = CHAT_SUCCEEDED;
            size_t length = strlen(reply);
            if (length > CHAT_REPLY_MAX) {
                length = CHAT_REPLY_MAX;
                while (length && ((unsigned char)reply[length] & 0xc0) == 0x80) length--;
            }
            memcpy(item->reply, reply, length); item->reply[length] = 0;
            snprintf(item->error, sizeof(item->error), "%s", strlen(reply) > length ? "REPLY_IN_HISTORY" : "");
        } else if (item->state != CHAT_UNCERTAIN) {
            item->state = CHAT_FAILED;
            snprintf(item->error, sizeof(item->error), "%s", error ? error : "AGENT_ERROR");
        }
        g_storage_error = persist();
    }
    if (item) item->executing = false;
    pthread_mutex_unlock(&g_chat_lock);
}
int portal_chat_clear(void)
{
    pthread_mutex_lock(&g_chat_lock); load(); expire();
    int ret = g_storage_error;
    for (int i = 0; !ret && i < CHAT_SLOTS; i++)
        if (g_chat[i].executing || (g_chat[i].used && g_chat[i].state == CHAT_QUEUED)) ret = -EBUSY;
    if (!ret) ret = session_clear("portal-admin");
    if (!ret) {
        for (int i = 0; i < CHAT_SLOTS; i++) { g_chat[i].content[0] = 0; g_chat[i].reply[0] = 0; }
        g_storage_error = persist(); ret = g_storage_error;
    }
    pthread_mutex_unlock(&g_chat_lock); return ret;
}
