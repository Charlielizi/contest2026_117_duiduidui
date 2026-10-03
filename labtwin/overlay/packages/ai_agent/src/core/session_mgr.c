/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "core/session_mgr.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "cJSON.h"

static const char *TAG = "session";

/* Session I/O must be serialized with truncate/clear for a given file, but
 * unrelated conversations should not wait behind another session's NAND I/O. */
#define SESSION_LOCK_STRIPES 8
static pthread_mutex_t s_session_locks[SESSION_LOCK_STRIPES] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t s_append_count_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_append_count;

static pthread_mutex_t *session_lock_for(const char *chat_id)
{
    const unsigned char *cursor = (const unsigned char *)chat_id;
    unsigned int hash = 2166136261U;

    while (*cursor) {
        hash ^= *cursor++;
        hash *= 16777619U;
    }

    return &s_session_locks[hash % SESSION_LOCK_STRIPES];
}

static void session_lock_all(void)
{
    for (int i = 0; i < SESSION_LOCK_STRIPES; i++) {
        pthread_mutex_lock(&s_session_locks[i]);
    }
}

static void session_unlock_all(void)
{
    for (int i = SESSION_LOCK_STRIPES - 1; i >= 0; i--) {
        pthread_mutex_unlock(&s_session_locks[i]);
    }
}

static void sanitize_filename(const char *src, char *dst, size_t dst_size)
{
    size_t i;

    for (i = 0; i < dst_size - 1 && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        /* Strict allowlist: only alphanumeric, hyphen, underscore, dot */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            dst[i] = (char)c;
        } else {
            dst[i] = '_';
        }
    }
    dst[i] = '\0';
}

static void session_path(const char *chat_id, char *buf, size_t size)
{
    char safe_id[64];

    sanitize_filename(chat_id, safe_id, sizeof(safe_id));
    snprintf(buf, size, "%s/tg_%s.jsonl", AGENT_SESSION_DIR, safe_id);
}

int session_mgr_init(void)
{
    syslog(LOG_INFO, "[%s] Session manager initialized at %s\n", TAG, AGENT_SESSION_DIR);
    return OK;
}

/**
 * Truncate session file to keep only the last max_lines entries.
 * Caller holds the selected session lock. rename() replaces the old file in
 * one directory operation, so a failed replacement retains the old history.
 */
static int session_truncate_locked(const char *chat_id, int max_lines)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));

    if (max_lines <= 0) {
        return ERROR;
    }

    /* Count lines first */
    FILE *f = fopen(path, "r");
    if (!f) {
        return OK;
    }

    int total_lines = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '\0' && line[0] != '\n') total_lines++;
    }

    if (total_lines <= max_lines) {
        fclose(f);
        return OK;
    }

    /* Need to truncate: rewind and skip old lines */
    rewind(f);
    int skip = total_lines - max_lines;
    int skipped = 0;
    while (skipped < skip && fgets(line, sizeof(line), f)) {
        if (line[0] != '\0' && line[0] != '\n') skipped++;
    }

    /* Write remaining lines to temp file */
    char tmp_path[140];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *tmp = fopen(tmp_path, "w");
    if (!tmp) {
        fclose(f);
        syslog(LOG_ERR, "[%s] Cannot create session temp file %s\n", TAG,
            tmp_path);
        return ERROR;
    }

    int write_failed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (fputs(line, tmp) == EOF) {
            write_failed = 1;
            break;
        }
    }

    fclose(f);
    if (fclose(tmp) != 0) {
        write_failed = 1;
    }

    if (write_failed) {
        remove(tmp_path);
        syslog(LOG_ERR, "[%s] Failed to write session temp file %s\n", TAG,
            tmp_path);
        return ERROR;
    }

    if (rename(tmp_path, path) != 0) {
        int err = errno;
        remove(tmp_path);
        syslog(LOG_ERR, "[%s] Cannot replace session %s: %d\n", TAG,
            chat_id, err);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Truncated session %s: %d → %d lines\n",
           TAG, chat_id, total_lines, max_lines);
    return OK;
}

int session_append(const char *chat_id, const char *role, const char *content)
{
    return session_append_request(chat_id, role, content, NULL);
}

int session_append_request(const char *chat_id, const char *role, const char *content, const char *request_id)
{
    if (!chat_id || !role || !content) {
        return ERROR;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return ERROR;
    }
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    if (request_id && request_id[0]) cJSON_AddStringToObject(obj, "request_id", request_id);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!line) {
        return ERROR;
    }

    char path[128];
    session_path(chat_id, path, sizeof(path));

    pthread_mutex_t *lock = session_lock_for(chat_id);
    pthread_mutex_lock(lock);
    FILE *f = fopen(path, "a");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot open session file %s\n", TAG, path);
        pthread_mutex_unlock(lock);
        free(line);
        return ERROR;
    }

    int result = OK;
    int write_failed = fprintf(f, "%s\n", line) < 0;
    if (fclose(f) != 0) {
        write_failed = 1;
    }
    if (write_failed) {
        syslog(LOG_ERR, "[%s] Cannot append session file %s\n", TAG, path);
        result = ERROR;
    }
    free(line);

    if (result == OK) {
        bool should_truncate = false;

        pthread_mutex_lock(&s_append_count_lock);
        if (++s_append_count >= 10) {
            s_append_count = 0;
            should_truncate = true;
        }
        pthread_mutex_unlock(&s_append_count_lock);

        if (should_truncate) {
            result = session_truncate_locked(chat_id,
                AGENT_SESSION_MAX_MSGS * 2);
        }
    }

    pthread_mutex_unlock(lock);
    return result;
}

int session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    if (!chat_id || !buf || size == 0 || max_msgs <= 0) {
        if (buf && size > 0) {
            snprintf(buf, size, "[]");
        }
        return ERROR;
    }

    pthread_mutex_t *lock = session_lock_for(chat_id);
    pthread_mutex_lock(lock);

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, size, "[]");
        pthread_mutex_unlock(lock);
        return OK;
    }

    /* Keep the HTTP worker stack bounded.  An 8 KiB line buffer plus the
     * message ring previously consumed most of its 12 KiB stack before the
     * JSON and REST call frames were included. */
    if (max_msgs > AGENT_SESSION_MAX_MSGS) max_msgs = AGENT_SESSION_MAX_MSGS;

    cJSON **messages = calloc((size_t)max_msgs, sizeof(*messages));
    char *line = malloc(8192);
    if (!messages || !line) {
        free(messages);
        free(line);
        fclose(f);
        snprintf(buf, size, "[]");
        pthread_mutex_unlock(lock);
        return ERROR;
    }
    int count = 0;
    int write_idx = 0;

    while (fgets(line, 8192, f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);
    free(line);

    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role    = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON_AddStringToObject(entry, "role",    role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
            cJSON *request = cJSON_GetObjectItem(src, "request_id");
            if (cJSON_IsString(request)) cJSON_AddStringToObject(entry, "request_id", request->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        cJSON_Delete(messages[(cleanup_start + i) % max_msgs]);
    }
    free(messages);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    pthread_mutex_unlock(lock);
    return OK;
}

int session_clear(const char *chat_id)
{
    if (!chat_id) {
        return ERROR;
    }

    char path[128];
    session_path(chat_id, path, sizeof(path));

    pthread_mutex_t *lock = session_lock_for(chat_id);
    pthread_mutex_lock(lock);
    if (remove(path) == 0) {
        syslog(LOG_INFO, "[%s] Session %s cleared\n", TAG, chat_id);
        pthread_mutex_unlock(lock);
        return OK;
    }
    pthread_mutex_unlock(lock);
    return ERROR;
}

int session_clear_all(void)
{
    session_lock_all();
    DIR *dir = opendir(AGENT_SESSION_DIR);
    if (!dir) {
        syslog(LOG_WARNING, "[%s] Cannot open session directory %s\n", TAG, AGENT_SESSION_DIR);
        session_unlock_all();
        return ERROR;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            char path[256];
            size_t dir_len = strlen(AGENT_SESSION_DIR);
            size_t name_len = strlen(entry->d_name);

            if (dir_len + 1 + name_len + 1 > sizeof(path)) {
                continue;
            }
            memcpy(path, AGENT_SESSION_DIR, dir_len);
            path[dir_len] = '/';
            memcpy(path + dir_len + 1, entry->d_name, name_len + 1);
            if (remove(path) == 0) {
                count++;
            }
        }
    }
    closedir(dir);
    session_unlock_all();

    syslog(LOG_INFO, "[%s] Cleared all sessions (%d files)\n", TAG, count);
    return OK;
}

void session_list(void)
{
    session_lock_all();
    DIR *dir = opendir(AGENT_SESSION_DIR);
    if (!dir) {
        dir = opendir(AGENT_DATA_DIR);
        if (!dir) {
            syslog(LOG_WARNING, "[%s] Cannot open data directory\n", TAG);
            session_unlock_all();
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            syslog(LOG_INFO, "[%s]   Session: %s\n", TAG, entry->d_name);
            count++;
        }
    }
    closedir(dir);
    session_unlock_all();

    if (count == 0) {
        syslog(LOG_INFO, "[%s]   No sessions found\n", TAG);
    }
}
