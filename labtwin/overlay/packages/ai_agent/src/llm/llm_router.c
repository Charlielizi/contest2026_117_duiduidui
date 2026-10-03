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

/**
 * LLM Router - Intelligent model routing inspired by ClawRouter
 *
 * Design borrowed from: https://github.com/BlockRunAI/ClawRouter
 * - Multi-backend support with automatic failover
 * - Complexity-based routing
 * - Cost optimization profiles (eco/auto/premium)
 */

#include <inttypes.h>

#include "agent_config.h"
#include "llm/llm_router.h"
#include "cJSON.h"
#include "infra/config_store.h"
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static const char* TAG = "llm_router";

/* Backend storage */
static llm_backend_t s_backends[LLM_ROUTER_MAX_BACKENDS];
static int s_backend_count = 0;
static llm_route_profile_t s_profile = LLM_ROUTE_AUTO;
static pthread_mutex_t s_router_lock = PTHREAD_MUTEX_INITIALIZER;

/* Failover threshold */
#define MAX_CONSECUTIVE_FAILURES 3

/* Auto-recovery: disabled backends retry after this many seconds */
#define RECOVERY_INTERVAL_SEC 300

/* Backoff: ignore rapid failures within this window (seconds) */
#define BACKOFF_DEBOUNCE_SEC 5

/* Complexity detection keywords */
static const char* s_complex_keywords[] = {
    "analyze", "explain", "implement", "debug", "refactor",
    "design", "architect", "optimize", "review", "compare",
    "分析", "解释", "实现", "调试", "重构", "设计", "优化",
    NULL
};

static const char* s_simple_keywords[] = {
    "hello", "hi", "thanks", "yes", "no", "ok", "help",
    "你好", "谢谢", "好的", "是", "否",
    NULL
};

/* ── Config keys ──────────────────────────────────────────── */

#define CFG_KEY_ROUTER_PROFILE "llm_router_profile"
#define CFG_KEY_BACKEND_PREFIX "llm_backend_"

/* Coding Plan exposes an OpenAI-compatible endpoint below.  The portal takes
 * a base URL, whereas llm_client takes a DNS host and a separate request path.
 * Keep that conversion at this boundary so settings from either UI work. */
static bool is_ark_coding_host(const char* host)
{
    return host && strstr(host, "ark.cn-beijing.volces.com") != NULL;
}

static const char* default_path_for_host(const char* host)
{
    return is_ark_coding_host(host) ? "/api/coding/v3/chat/completions" :
        AGENT_LLM_API_PATH;
}

/* Convert a user-facing base URL such as
 * https://ark.cn-beijing.volces.com/api/coding/v3 into the form the transport
 * requires: host=ark.cn-beijing.volces.com and an absolute request path.
 * Returns true when the backend was changed. */
static bool normalize_backend_endpoint(llm_backend_t* backend)
{
    char host[sizeof(backend->host)] = { 0 };
    char base_path[sizeof(backend->path)] = { 0 };
    char path[sizeof(backend->path)] = { 0 };
    const char* source;
    const char* slash;
    size_t host_len;
    size_t base_len;
    bool changed = false;

    if (!backend || !backend->host[0]) {
        return false;
    }

    source = backend->host;
    if (strncmp(source, "https://", 8) == 0) {
        source += 8;
    } else if (strncmp(source, "http://", 7) == 0) {
        source += 7;
    }

    slash = strchr(source, '/');
    host_len = slash ? (size_t)(slash - source) : strlen(source);
    if (host_len == 0 || host_len >= sizeof(host)) {
        return false;
    }
    memcpy(host, source, host_len);
    host[host_len] = '\0';

    if (slash && slash[0] != '\0') {
        snprintf(base_path, sizeof(base_path), "%s", slash);
        base_len = strlen(base_path);
        while (base_len > 1 && base_path[base_len - 1] == '/') {
            base_path[--base_len] = '\0';
        }
    }

    if (strcmp(backend->host, host) != 0) {
        snprintf(backend->host, sizeof(backend->host), "%s", host);
        changed = true;
    }

    if (is_ark_coding_host(backend->host) &&
        (!backend->path[0] ||
         strcmp(backend->path, AGENT_LLM_API_PATH) == 0 ||
         strcmp(backend->path, "/chat/completions") == 0)) {
        snprintf(path, sizeof(path), "%s", default_path_for_host(backend->host));
    } else if (base_path[0] &&
               strncmp(backend->path, base_path, strlen(base_path)) != 0) {
        snprintf(path, sizeof(path), "%s%s", base_path,
            backend->path[0] ? backend->path : "/");
    } else if (!backend->path[0]) {
        snprintf(path, sizeof(path), "%s", default_path_for_host(backend->host));
    }

    if (path[0] && strcmp(backend->path, path) != 0) {
        snprintf(backend->path, sizeof(backend->path), "%s", path);
        changed = true;
    }
    return changed;
}

/* The administrator portal originally stored these fields separately. */
static bool load_legacy_primary_backend(llm_backend_t* backend)
{
    char host[sizeof(backend->host)] = { 0 };
    char path[sizeof(backend->path)] = { 0 };
    char port[sizeof(backend->port)] = { 0 };
    char api_key[sizeof(backend->api_key)] = { 0 };
    char model[sizeof(backend->model)] = { 0 };

    if (!backend ||
        claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host)) != 0 ||
        claw_config_get(AGENT_CFG_KEY_API_KEY, api_key, sizeof(api_key)) != 0 ||
        claw_config_get(AGENT_CFG_KEY_MODEL, model, sizeof(model)) != 0 ||
        !host[0] || !api_key[0] || !model[0]) {
        return false;
    }

    if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, path, sizeof(path)) != 0 ||
        !path[0]) {
        snprintf(path, sizeof(path), "%s", default_path_for_host(host));
    }
    if (claw_config_get("llm_port", port, sizeof(port)) != 0 || !port[0]) {
        snprintf(port, sizeof(port), "443");
    }

    memset(backend, 0, sizeof(*backend));
    snprintf(backend->host, sizeof(backend->host), "%s", host);
    snprintf(backend->path, sizeof(backend->path), "%s", path);
    snprintf(backend->port, sizeof(backend->port), "%s", port);
    snprintf(backend->api_key, sizeof(backend->api_key), "%s", api_key);
    snprintf(backend->model, sizeof(backend->model), "%s", model);
    backend->enabled = true;
    backend->priority = 0;
    backend->cost_tier = 1;
    normalize_backend_endpoint(backend);
    return true;
}

/* ── Helper: check if string contains keyword ─────────────── */

static bool contains_keyword(const char* text, const char** keywords)
{
    if (!text || !keywords) {
        return false;
    }
    for (int i = 0; keywords[i] != NULL; i++) {
        if (strcasestr(text, keywords[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* ── Init ─────────────────────────────────────────────────── */

int llm_router_init(void)
{
    bool migrate_legacy_primary = false;
    bool normalize_primary_endpoint = false;
    llm_backend_t normalized_primary = { 0 };

    pthread_mutex_lock(&s_router_lock);

    /* Load profile from config */
    char tmp[32] = { 0 };
    if (claw_config_get(CFG_KEY_ROUTER_PROFILE, tmp, sizeof(tmp)) == 0) {
        if (strcmp(tmp, "eco") == 0) {
            s_profile = LLM_ROUTE_ECO;
        } else if (strcmp(tmp, "premium") == 0) {
            s_profile = LLM_ROUTE_PREMIUM;
        } else {
            s_profile = LLM_ROUTE_AUTO;
        }
    }

    /* Load backends from config.  Reinitialisation can happen after a
     * settings update, so do not leave a removed backend active in RAM. */
    memset(s_backends, 0, sizeof(s_backends));
    s_backend_count = 0;
    for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
        char key[64];
        char json[1024] = { 0 };

        snprintf(key, sizeof(key), "%s%d", CFG_KEY_BACKEND_PREFIX, i);
        if (claw_config_get(key, json, sizeof(json)) != 0 || json[0] == '\0') {
            continue;
        }

        cJSON* obj = cJSON_Parse(json);
        if (!obj) {
            continue;
        }

        llm_backend_t* b = &s_backends[i];
        memset(b, 0, sizeof(*b));

        cJSON* item;
        if ((item = cJSON_GetObjectItem(obj, "host")) && cJSON_IsString(item)) {
            strncpy(b->host, item->valuestring, sizeof(b->host) - 1);
        }
        if ((item = cJSON_GetObjectItem(obj, "path")) && cJSON_IsString(item)) {
            strncpy(b->path, item->valuestring, sizeof(b->path) - 1);
        }
        if ((item = cJSON_GetObjectItem(obj, "port")) && cJSON_IsString(item)) {
            strncpy(b->port, item->valuestring, sizeof(b->port) - 1);
        }
        if ((item = cJSON_GetObjectItem(obj, "api_key")) && cJSON_IsString(item)) {
            strncpy(b->api_key, item->valuestring, sizeof(b->api_key) - 1);
        }
        if ((item = cJSON_GetObjectItem(obj, "model")) && cJSON_IsString(item)) {
            strncpy(b->model, item->valuestring, sizeof(b->model) - 1);
        }
        if ((item = cJSON_GetObjectItem(obj, "priority")) && cJSON_IsNumber(item)) {
            b->priority = item->valueint;
        }
        if ((item = cJSON_GetObjectItem(obj, "cost_tier")) && cJSON_IsNumber(item)) {
            b->cost_tier = item->valueint;
        }

        /* Upgrade previously persisted portal values as well.  The old
         * migration stored a full URL in host, which made the TLS transport
         * attempt DNS resolution of a string containing https:// and /path. */
        if (i == 0 && normalize_backend_endpoint(b)) {
            normalized_primary = *b;
            normalize_primary_endpoint = true;
        }

        b->enabled = true;
        b->fail_count = 0;
        s_backend_count++;

        cJSON_Delete(obj);
        syslog(LOG_INFO, "[%s] Loaded backend %d: %s (model: %s, tier: %d)\n",
            TAG, i, b->host, b->model, b->cost_tier);
    }

    migrate_legacy_primary = s_backend_count == 0;

    pthread_mutex_unlock(&s_router_lock);

    /* Existing portal users have flat api_key/model/llm_host keys but no
     * llm_backend_0.  Migrate exactly once and apply the result now, so a
     * reboot is not needed after saving the settings page. */
    if (migrate_legacy_primary && llm_router_sync_primary_from_legacy() == 0) {
        syslog(LOG_INFO, "[%s] Migrated legacy primary LLM configuration\n", TAG);
    } else if (normalize_primary_endpoint &&
               llm_router_set_backend(0, &normalized_primary) == 0 &&
               llm_router_apply(0) == 0) {
        syslog(LOG_INFO, "[%s] Normalized primary LLM endpoint URL\n", TAG);
    }

    syslog(LOG_INFO, "[%s] Router initialized: %d backends, profile=%d\n",
        TAG, s_backend_count, s_profile);
    return 0;
}

int llm_router_sync_primary_from_legacy(void)
{
    llm_backend_t backend;

    if (!load_legacy_primary_backend(&backend)) {
        return -1;
    }
    if (llm_router_set_backend(0, &backend) != 0) {
        return -1;
    }
    return llm_router_apply(0);
}

/* ── Backend management ───────────────────────────────────── */

int llm_router_set_backend(int index, const llm_backend_t* backend)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS || !backend) {
        return -1;
    }

    pthread_mutex_lock(&s_router_lock);
    memcpy(&s_backends[index], backend, sizeof(llm_backend_t));
    s_backends[index].fail_count = 0;

    /* Save to config */
    cJSON* obj = cJSON_CreateObject();
    if (!obj) {
        pthread_mutex_unlock(&s_router_lock);
        return -1;
    }
    cJSON_AddStringToObject(obj, "host", backend->host);
    cJSON_AddStringToObject(obj, "path", backend->path);
    cJSON_AddStringToObject(obj, "port", backend->port);
    cJSON_AddStringToObject(obj, "api_key", backend->api_key);
    cJSON_AddStringToObject(obj, "model", backend->model);
    cJSON_AddNumberToObject(obj, "priority", backend->priority);
    cJSON_AddNumberToObject(obj, "cost_tier", backend->cost_tier);

    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    obj = NULL;

    if (json) {
        char key[64];
        snprintf(key, sizeof(key), "%s%d", CFG_KEY_BACKEND_PREFIX, index);
        claw_config_set(key, json);
        free(json);
        json = NULL;
    }

    if (index >= s_backend_count) {
        s_backend_count = index + 1;
    }

    pthread_mutex_unlock(&s_router_lock);

    syslog(LOG_INFO, "[%s] Backend %d configured: %s\n", TAG, index, backend->host);
    return 0;
}

int llm_router_get_backend(int index, llm_backend_t* backend)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS || !backend) {
        return -1;
    }

    pthread_mutex_lock(&s_router_lock);
    if (!s_backends[index].enabled || s_backends[index].host[0] == '\0') {
        pthread_mutex_unlock(&s_router_lock);
        return -1;
    }
    memcpy(backend, &s_backends[index], sizeof(llm_backend_t));
    pthread_mutex_unlock(&s_router_lock);

    return 0;
}

/* ── Profile management ───────────────────────────────────── */

void llm_router_set_profile(llm_route_profile_t profile)
{
    pthread_mutex_lock(&s_router_lock);
    s_profile = profile;

    const char* name = "auto";
    if (profile == LLM_ROUTE_ECO) {
        name = "eco";
    } else if (profile == LLM_ROUTE_PREMIUM) {
        name = "premium";
    }
    claw_config_set(CFG_KEY_ROUTER_PROFILE, name);

    pthread_mutex_unlock(&s_router_lock);
    syslog(LOG_INFO, "[%s] Profile set to: %s\n", TAG, name);
}

llm_route_profile_t llm_router_get_profile(void)
{
    llm_route_profile_t p;
    pthread_mutex_lock(&s_router_lock);
    p = s_profile;
    pthread_mutex_unlock(&s_router_lock);
    return p;
}

/* ── Complexity estimation ────────────────────────────────── */

llm_complexity_t llm_router_estimate_complexity(const char* prompt, size_t prompt_len)
{
    if (!prompt || prompt_len == 0) {
        return LLM_COMPLEXITY_SIMPLE;
    }

    int score = 0;

    /* Length-based scoring: rough token estimate (1 token ~ 4 chars) */
    size_t est_tokens = prompt_len / 4;
    if (est_tokens > 200) {
        score += 3;
    } else if (est_tokens > 50) {
        score += 1;
    }

    /* Question mark count — multiple questions suggest complexity */
    int qmarks = 0;
    for (size_t i = 0; i < prompt_len; i++) {
        if (prompt[i] == '?' || (i + 2 < prompt_len && (unsigned char)prompt[i] == 0xEF && (unsigned char)prompt[i + 1] == 0xBC && (unsigned char)prompt[i + 2] == 0x9F)) {
            qmarks++;
        }
    }
    if (qmarks >= 3) {
        score += 2;
    } else if (qmarks >= 1) {
        score += 1;
    }

    /* Code block detection (``` or indented code) */
    if (strstr(prompt, "```") != NULL) {
        score += 3;
    }

    /* Complex keyword detection */
    if (contains_keyword(prompt, s_complex_keywords)) {
        score += 2;
    }

    /* Simple keyword shortcut — only if nothing else triggers */
    if (score == 0 && prompt_len < 50
        && contains_keyword(prompt, s_simple_keywords)) {
        return LLM_COMPLEXITY_SIMPLE;
    }

    /* Score thresholds */
    if (score >= 4) {
        return LLM_COMPLEXITY_COMPLEX;
    } else if (score >= 2) {
        return LLM_COMPLEXITY_MEDIUM;
    }
    return LLM_COMPLEXITY_SIMPLE;
}

/* ── Backend selection (internal) ──────────────────────────── */

static int router_select_internal(llm_complexity_t complexity,
    llm_route_profile_t profile)
{
    int best_idx = -1;
    int best_score = -1;
    int fallback_idx = -1;          /* least-backed-off backend */
    uint32_t fallback_until = UINT32_MAX;
    uint32_t now = (uint32_t)time(NULL);

    for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
        llm_backend_t* b = &s_backends[i];

        /* Skip disabled or unconfigured backends */
        if (!b->enabled || b->host[0] == '\0') {
            continue;
        }

        /* Skip backends in backoff period, but track the one
         * with the shortest remaining backoff as fallback. */
        if (b->backoff_until > now) {
            if (b->backoff_until < fallback_until) {
                fallback_until = b->backoff_until;
                fallback_idx = i;
            }
            continue;
        }

        /* Auto-recovery: reset backends that have been disabled long enough */
        if (b->fail_count >= MAX_CONSECUTIVE_FAILURES) {
            if (now - b->last_fail_ts >= RECOVERY_INTERVAL_SEC) {
                syslog(LOG_INFO, "[%s] Auto-recovering backend %d after %ds\n",
                    TAG, i, RECOVERY_INTERVAL_SEC);
                b->fail_count = 0;
            } else {
                continue;
            }
        }

        /* Calculate score based on profile and complexity */
        int score = 100 - b->priority * 10;

        switch (profile) {
        case LLM_ROUTE_ECO:
            /* Prefer cheapest (lower cost_tier = higher score) */
            score += (3 - b->cost_tier) * 30;
            break;

        case LLM_ROUTE_PREMIUM:
            /* Prefer best quality (higher cost_tier = higher score) */
            score += b->cost_tier * 30;
            break;

        case LLM_ROUTE_AUTO:
        default:
            /* Balance: match cost_tier to complexity */
            if (complexity == LLM_COMPLEXITY_SIMPLE && b->cost_tier <= 1) {
                score += 20;
            } else if (complexity == LLM_COMPLEXITY_MEDIUM && b->cost_tier == 1) {
                score += 20;
            } else if (complexity == LLM_COMPLEXITY_COMPLEX && b->cost_tier >= 2) {
                score += 20;
            }
            break;
        }

        /* Penalize backends with recent failures */
        score -= b->fail_count * 15;

        /* Slight preference for lower-latency backends */
        if (b->avg_latency_ms > 0 && b->avg_latency_ms < 5000) {
            score += (5000 - (int)b->avg_latency_ms) / 500;
        }

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        /* All backends in backoff — use the one recovering soonest */
        best_idx = fallback_idx;
        if (best_idx >= 0) {
            syslog(LOG_INFO, "[%s] All backends backed off, using fallback %d\n",
                TAG, best_idx);
        } else {
            syslog(LOG_WARNING, "[%s] No available backend\n", TAG);
        }
    }

    return best_idx;
}

/* ── Backend selection (public API) ───────────────────────── */

int llm_router_select(llm_complexity_t complexity)
{
    pthread_mutex_lock(&s_router_lock);
    int idx = router_select_internal(complexity, s_profile);
    pthread_mutex_unlock(&s_router_lock);
    return idx;
}

int llm_router_select_with_profile(llm_complexity_t complexity,
    llm_route_profile_t profile)
{
    pthread_mutex_lock(&s_router_lock);
    int idx = router_select_internal(complexity, profile);
    pthread_mutex_unlock(&s_router_lock);
    return idx;
}

/* ── Failure tracking ─────────────────────────────────────── */

void llm_router_report_failure(int index)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS) {
        return;
    }

    pthread_mutex_lock(&s_router_lock);

    llm_backend_t* b = &s_backends[index];
    uint32_t now = (uint32_t)time(NULL);

    /* Debounce: only count if outside rapid-fire window */
    if (now - b->last_fail_ts >= BACKOFF_DEBOUNCE_SEC) {
        b->fail_count++;
    }
    b->last_fail_ts = now;
    b->total_failures++;

    /* Exponential backoff starting from the 2nd failure.
     * First failure gets no backoff — transient 502/503 from load
     * balancers are common and usually resolve within seconds.
     * Backoff schedule: 0s, 2s, 4s, 8s, ... up to 60s max. */
    uint32_t backoff = 0;
    if (b->fail_count >= 2) {
        uint32_t shift = b->fail_count - 2;
        if (shift > 5) shift = 5;  /* cap shift to avoid UB */
        backoff = (1u << shift) * 2;
        if (backoff > 60) {
            backoff = 60;
        }
    }
    b->backoff_until = backoff > 0 ? now + backoff : 0;

    syslog(LOG_WARNING, "[%s] Backend %d failure count: %d (backoff %" PRIu32 ")\n",
        TAG, index, b->fail_count, backoff);

    pthread_mutex_unlock(&s_router_lock);
}

void llm_router_report_success(int index)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS) {
        return;
    }

    pthread_mutex_lock(&s_router_lock);
    s_backends[index].fail_count = 0;
    s_backends[index].backoff_until = 0;
    s_backends[index].total_calls++;
    pthread_mutex_unlock(&s_router_lock);
}

/* ── Latency tracking ─────────────────────────────────────── */

void llm_router_report_latency(int index, uint32_t latency_ms)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS) {
        return;
    }

    pthread_mutex_lock(&s_router_lock);
    llm_backend_t* b = &s_backends[index];

    /* Exponential moving average: new = 0.7 * old + 0.3 * sample */
    if (b->avg_latency_ms == 0) {
        b->avg_latency_ms = latency_ms;
    } else {
        b->avg_latency_ms = (b->avg_latency_ms * 7 + latency_ms * 3) / 10;
    }
    pthread_mutex_unlock(&s_router_lock);
}

/* ── Apply backend to llm_proxy ───────────────────────────── */

int llm_router_apply(int index)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS) {
        return -1;
    }

    llm_backend_t b;
    pthread_mutex_lock(&s_router_lock);
    memcpy(&b, &s_backends[index], sizeof(b));
    pthread_mutex_unlock(&s_router_lock);

    if (b.host[0] == '\0') {
        return -1;
    }

    /* Atomic update — single lock acquisition in llm_proxy */
    llm_set_all(b.host, b.path, b.port, b.api_key, b.model);

    return 0;
}

void llm_router_report_tokens(int index, int prompt_tokens,
    int completion_tokens)
{
    if (index < 0 || index >= LLM_ROUTER_MAX_BACKENDS) {
        return;
    }

    pthread_mutex_lock(&s_router_lock);
    llm_backend_t* b = &s_backends[index];
    if (prompt_tokens > 0) {
        b->total_prompt_tokens += (uint32_t)prompt_tokens;
    }
    if (completion_tokens > 0) {
        b->total_completion_tokens += (uint32_t)completion_tokens;
    }
    pthread_mutex_unlock(&s_router_lock);
}

/* ── Status JSON ──────────────────────────────────────────── */

char* llm_router_status_json(void)
{
    pthread_mutex_lock(&s_router_lock);
    uint32_t now = (uint32_t)time(NULL);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        pthread_mutex_unlock(&s_router_lock);
        return NULL;
    }

    /* Profile */
    const char* profile_name = "auto";
    if (s_profile == LLM_ROUTE_ECO) {
        profile_name = "eco";
    } else if (s_profile == LLM_ROUTE_PREMIUM) {
        profile_name = "premium";
    }
    cJSON_AddStringToObject(root, "profile", profile_name);
    cJSON_AddNumberToObject(root, "backend_count", s_backend_count);

    /* Backends array */
    cJSON* backends = cJSON_CreateArray();
    if (!backends) {
        cJSON_Delete(root);
        pthread_mutex_unlock(&s_router_lock);
        return NULL;
    }
    for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
        llm_backend_t* b = &s_backends[i];
        if (b->host[0] == '\0') {
            continue;
        }

        cJSON* item = cJSON_CreateObject();
        if (!item) {
            continue; /* Skip this backend on OOM */
        }
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "host", b->host);
        cJSON_AddStringToObject(item, "model", b->model);
        cJSON_AddNumberToObject(item, "priority", b->priority);
        cJSON_AddNumberToObject(item, "cost_tier", b->cost_tier);
        cJSON_AddBoolToObject(item, "enabled", b->enabled);
        cJSON_AddNumberToObject(item, "fail_count", b->fail_count);
        cJSON_AddNumberToObject(item, "total_calls", b->total_calls);
        cJSON_AddNumberToObject(item, "total_failures", b->total_failures);
        cJSON_AddNumberToObject(item, "avg_latency_ms", b->avg_latency_ms);
        cJSON_AddNumberToObject(item, "total_prompt_tokens",
            b->total_prompt_tokens);
        cJSON_AddNumberToObject(item, "total_completion_tokens",
            b->total_completion_tokens);

        const char* status = "ok";
        if (b->fail_count >= MAX_CONSECUTIVE_FAILURES) {
            if (now - b->last_fail_ts >= RECOVERY_INTERVAL_SEC) {
                status = "recovering";
            } else {
                status = "disabled";
            }
        } else if (b->backoff_until > now) {
            status = "backoff";
        } else if (b->fail_count > 0) {
            status = "degraded";
        }
        cJSON_AddStringToObject(item, "status", status);

        cJSON_AddItemToArray(backends, item);
    }
    cJSON_AddItemToObject(root, "backends", backends);

    /* Global token summary (computed while still holding lock) */
    uint32_t total_prompt = 0;
    uint32_t total_completion = 0;
    uint32_t total_calls_all = 0;
    uint32_t total_failures_all = 0;
    for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
        llm_backend_t* b = &s_backends[i];
        if (b->host[0] == '\0') {
            continue;
        }
        total_prompt += b->total_prompt_tokens;
        total_completion += b->total_completion_tokens;
        total_calls_all += b->total_calls;
        total_failures_all += b->total_failures;
    }

    pthread_mutex_unlock(&s_router_lock);

    /* Token summary */
    cJSON* tokens = cJSON_CreateObject();
    if (tokens) {
        cJSON_AddNumberToObject(tokens, "total_prompt_tokens", total_prompt);
        cJSON_AddNumberToObject(tokens, "total_completion_tokens",
            total_completion);
        cJSON_AddNumberToObject(tokens, "total_tokens",
            total_prompt + total_completion);
        cJSON_AddItemToObject(root, "token_summary", tokens);
    }

    /* Cache statistics (cache has its own lock) */
    cJSON* cache = cJSON_CreateObject();
    if (cache) {
        uint32_t hits = llm_cache_hit_count();
        uint32_t saved = llm_cache_tokens_saved();
        cJSON_AddNumberToObject(cache, "hits", hits);
        cJSON_AddNumberToObject(cache, "tokens_saved", saved);
        cJSON_AddItemToObject(root, "cache", cache);
    }

    /* Availability summary */
    cJSON* avail = cJSON_CreateObject();
    if (avail) {
        cJSON_AddNumberToObject(avail, "total_calls", total_calls_all);
        cJSON_AddNumberToObject(avail, "total_failures", total_failures_all);
        if (total_calls_all > 0) {
            double success_rate = 100.0
                * (total_calls_all - total_failures_all) / total_calls_all;
            cJSON_AddNumberToObject(avail, "success_rate_pct", success_rate);
        }
        cJSON_AddItemToObject(root, "availability", avail);
    }

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    return json;
}
