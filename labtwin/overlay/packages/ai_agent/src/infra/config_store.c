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

#include "config_store.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cutils/properties.h>
#include <kvdb.h>
#include "cJSON.h"

static const char *TAG = "cfgstore";

#define CONFIG_PROPERTY_PREFIX "persist.ai_agent."
#define CONFIG_VALUE_PREFIX "v1:"
#define CONFIG_DELETE_MARKER "d1"
#define CONFIG_PROPERTY_VOLC_ASR_CLUSTER "volc.asr.c"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *s_cache;

/* ── helpers ─────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    /* Do not chmod an existing YAFFS directory on every setting update.
     * The R528 NAND driver can block indefinitely on metadata-only chmod()
     * operations while network services are active.  Newly created paths
     * already receive 0700 from mkdir(). */
    return OK;
}

static cJSON *load_json(void)
{
    FILE *f = fopen(AGENT_CONFIG_FILE, "r");
    if (!f) return cJSON_CreateObject();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return cJSON_CreateObject(); }

    char *buf = (char *)malloc((size_t)(sz + 1));
    if (!buf) { fclose(f); return cJSON_CreateObject(); }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root ? root : cJSON_CreateObject();
}

static int config_property_key(const char *key, char *output,
                               size_t output_size)
{
    const char *storage_key;
    int length;

    if (!key || !key[0] || !output || output_size == 0)
        return ERROR;

    /* Keep the public configuration key stable while storing it under a
     * short KVDB basename.  The target has NAME_MAX=32, and the ordinary
     * persist.ai_agent. prefix makes volc_asr_cluster one character too
     * long to persist. */
    storage_key = strcmp(key, AGENT_CFG_KEY_VOLC_ASR_CLUSTER) == 0
        ? CONFIG_PROPERTY_VOLC_ASR_CLUSTER
        : key;

    length = snprintf(output, output_size, "%s%s",
                      CONFIG_PROPERTY_PREFIX, storage_key);
    return length > 0 && (size_t)length < output_size &&
           length < PROPERTY_KEY_MAX ? OK : ERROR;
}

/* ── public API ──────────────────────────────────────────────── */

int config_store_init(void)
{
    mkdirs(AGENT_DATA_DIR);
    mkdirs(AGENT_CONFIG_DIR);
    mkdirs(AGENT_MEMORY_DIR);
    mkdirs(AGENT_SESSION_DIR);
    pthread_mutex_lock(&s_lock);
    if (!s_cache)
        s_cache = load_json();
    pthread_mutex_unlock(&s_lock);
    syslog(LOG_INFO, "[%s] Config store ready (KVDB/NVS with read-only legacy %s)\n",
           TAG, AGENT_CONFIG_FILE);
    return OK;
}

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    char property_key[PROPERTY_KEY_MAX];
    char property_value[PROPERTY_VALUE_MAX] = { 0 };
    int property_length;

    if (!buf || buf_size == 0 ||
        config_property_key(key, property_key, sizeof(property_key)) != OK)
        return ERROR;

    property_length = property_get_with_err(property_key, property_value);
    if (property_length > 0) {
        if (strcmp(property_value, CONFIG_DELETE_MARKER) == 0)
            return ERROR;
        if (strncmp(property_value, CONFIG_VALUE_PREFIX,
                    strlen(CONFIG_VALUE_PREFIX)) == 0) {
            const char *value = property_value + strlen(CONFIG_VALUE_PREFIX);
            if (!value[0])
                return ERROR;
            strncpy(buf, value, buf_size - 1);
            buf[buf_size - 1] = '\0';
            return OK;
        }
    }

    /* Compatibility only: legacy YAFFS JSON is never rewritten. */
    pthread_mutex_lock(&s_lock);
    if (!s_cache) s_cache = load_json();
    cJSON *item = cJSON_GetObjectItem(s_cache, key);
    int ret = ERROR;
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strncpy(buf, item->valuestring, buf_size - 1);
        buf[buf_size - 1] = '\0';
        ret = OK;
    }
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int claw_config_set(const char *key, const char *value)
{
    char property_key[PROPERTY_KEY_MAX];
    char property_value[PROPERTY_VALUE_MAX];
    int length;
    int ret;

    value = value ? value : "";
    if (config_property_key(key, property_key, sizeof(property_key)) != OK)
        return ERROR;
    length = snprintf(property_value, sizeof(property_value), "%s%s",
                      CONFIG_VALUE_PREFIX, value);
    if (length < 0 || length >= (int)sizeof(property_value))
        return ERROR;

    ret = property_set(property_key, property_value);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] KVDB set failed for %s: %d\n",
               TAG, key, ret);
        return ERROR;
    }

    pthread_mutex_lock(&s_lock);
    if (!s_cache) s_cache = load_json();
    cJSON_DeleteItemFromObject(s_cache, key);
    cJSON_AddStringToObject(s_cache, key, value);
    pthread_mutex_unlock(&s_lock);
    return OK;
}

int config_del(const char *key)
{
    char property_key[PROPERTY_KEY_MAX];
    int ret;

    if (config_property_key(key, property_key, sizeof(property_key)) != OK)
        return ERROR;
    ret = property_set(property_key, CONFIG_DELETE_MARKER);
    if (ret < 0)
        return ERROR;

    pthread_mutex_lock(&s_lock);
    if (!s_cache) s_cache = load_json();
    cJSON_DeleteItemFromObject(s_cache, key);
    pthread_mutex_unlock(&s_lock);
    return OK;
}

int config_erase_all(void)
{
    cJSON *item;
    int ret = OK;

    pthread_mutex_lock(&s_lock);
    if (!s_cache)
        s_cache = load_json();
    cJSON_ArrayForEach(item, s_cache) {
        char property_key[PROPERTY_KEY_MAX];
        if (item->string &&
            config_property_key(item->string, property_key,
                                sizeof(property_key)) == OK &&
            property_set(property_key, CONFIG_DELETE_MARKER) < 0)
            ret = ERROR;
    }
    cJSON_Delete(s_cache);
    s_cache = cJSON_CreateObject();
    pthread_mutex_unlock(&s_lock);
    return ret;
}
