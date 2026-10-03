#include "labtwin/labtwin.h"
#include "labtwin/labtwin_environment.h"
#include "cJSON.h"
#ifndef LABTWIN_ENV_HOST_TEST
#include "infra/config_store.h"
#include <kvdb.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define TAG "labtwin_env"
#ifndef LABTWIN_ROOT
#ifdef LABTWIN_ENV_HOST_TEST
#define LABTWIN_ROOT "/tmp/labtwin-m4-environment-test"
#else
#define LABTWIN_ROOT "/data/labtwin"
#endif
#endif
#define ENV_ROOT LABTWIN_ROOT "/environment"
#define ENV_CONFIG ENV_ROOT "/config.json"
/* KVDB_FILE maps keys directly to YAFFS file names.  Keep both names below
 * the filesystem component limit or service initialization fails with
 * ENAMETOOLONG before the UI command layer becomes available. */
#define ENV_CONFIG_KEY "lt.env.rules"
#define ENV_PROPERTY_KEY "persist.lt.env.rules.v1"
/* Read prior releases once and rewrite a valid value under the shorter key. */
#define ENV_LEGACY_CONFIG_KEY "labtwin.environment.rules"
#define ENV_LEGACY_PROPERTY_KEY "persist.labtwin.environment.rules.v1"
#define ENV_SNAPSHOT ENV_ROOT "/snapshot.json"
#define ENV_EVENTS ENV_ROOT "/events.jsonl"
#define ENV_TELEMETRY ENV_ROOT "/telemetry.jsonl"
#define ENV_TELEMETRY_HOURLY ENV_ROOT "/telemetry.hourly.jsonl"
#define ENV_TELEMETRY_HOURLY_TMP ENV_ROOT "/telemetry.hourly.tmp"
#define ENV_SCHEMA_VERSION 1
#define ENV_MAX_EVENTS 64
#define ENV_TELEMETRY_RECENT_MAX (7U * 24U * 12U)
#define ENV_TELEMETRY_HOURLY_MAX (23U * 24U)
#define ENV_TELEMETRY_MAX (ENV_TELEMETRY_RECENT_MAX + ENV_TELEMETRY_HOURLY_MAX)
#define ENV_TELEMETRY_EMIT_MAX 160U
#define ENV_TELEMETRY_WINDOW_MS (5ULL * 60ULL * 1000ULL)
#define ENV_TELEMETRY_HOUR_SECONDS 3600LL
#define ENV_TRIGGER_SAMPLES 5
#define ENV_CLEAR_SAMPLES 10
#define ENV_STALE_MS 5000ULL
#define ENV_COOLDOWN_MS 60000ULL
#define ENV_NOTICE_MS 5000ULL
#define ENV_LINE_SIZE 2048
#define ENV_TELEMETRY_LINE_SIZE 256

typedef enum {
    SENSOR_TEMPERATURE = 0,
    SENSOR_HUMIDITY
} env_sensor_t;

typedef enum {
    RULE_HIGH = 0,
    RULE_LOW
} env_direction_t;

typedef struct {
    const char *id;
    env_sensor_t sensor;
    env_direction_t direction;
    float trigger;
    float clear;
    unsigned int pending_count;
    unsigned int clear_count;
    uint64_t last_sample_seq;
    uint64_t cooldown_until_ms;
    int64_t cooldown_until_epoch;
} env_rule_t;

typedef struct {
    bool used;
    labtwin_environment_event_view_t view;
} env_event_t;

typedef struct {
    bool valid;
    float value;
    uint64_t sampled_ms;
    uint64_t seq;
    uint32_t failures;
} env_sample_t;

typedef struct {
    uint64_t seq;
    int64_t timestamp_epoch;
    float temperature_avg;
    float humidity_avg;
    uint32_t valid_samples;
    uint32_t stale_samples;
    bool temperature_valid;
    bool humidity_valid;
} env_telemetry_t;

static pthread_mutex_t g_env_lock = PTHREAD_MUTEX_INITIALIZER;
#ifndef LABTWIN_ENV_HOST_TEST
static pthread_t g_env_thread;
#endif
static bool g_initialized;
static bool g_running;
static bool g_storage_error;
static bool g_config_fallback;
static uint64_t g_event_seq;
static env_sample_t g_samples[2];
static env_telemetry_t g_telemetry[ENV_TELEMETRY_MAX];
static size_t g_telemetry_start;
static size_t g_telemetry_count;
static uint64_t g_telemetry_seq;
static unsigned int g_telemetry_active_count;
static unsigned int g_telemetry_hourly_count;
static uint64_t g_telemetry_window_until_ms;
static float g_telemetry_temperature_sum;
static float g_telemetry_humidity_sum;
static uint32_t g_telemetry_temperature_count;
static uint32_t g_telemetry_humidity_count;
static uint32_t g_telemetry_stale_count;
static bool g_injected[2];
#if !defined(LABTWIN_ENV_HOST_TEST) || defined(CONFIG_AI_AGENT_LABTWIN_ENV_TEST)
static float g_injected_value[2];
#endif
static env_event_t g_events[ENV_MAX_EVENTS];
static char g_foreground[32];
static int g_notice_index = -1;
static uint64_t g_notice_until_ms;

static void load_telemetry_locked(void);

static env_rule_t g_rules[LABTWIN_ENV_RULE_COUNT] = {
    {.id = "TEMP_HIGH", .sensor = SENSOR_TEMPERATURE,
     .direction = RULE_HIGH, .trigger = 35.0f, .clear = 33.0f},
    {.id = "TEMP_LOW", .sensor = SENSOR_TEMPERATURE,
     .direction = RULE_LOW, .trigger = 10.0f, .clear = 12.0f},
    {.id = "HUMIDITY_HIGH", .sensor = SENSOR_HUMIDITY,
     .direction = RULE_HIGH, .trigger = 70.0f, .clear = 65.0f},
    {.id = "HUMIDITY_LOW", .sensor = SENSOR_HUMIDITY,
     .direction = RULE_LOW, .trigger = 20.0f, .clear = 25.0f},
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int64_t wall_epoch(void)
{
    return (int64_t)time(NULL);
}

static const char *state_name(labtwin_environment_event_state_t state)
{
    switch (state) {
    case LABTWIN_ENV_EVENT_ACTIVE: return "ACTIVE";
    case LABTWIN_ENV_EVENT_ACKNOWLEDGED: return "ACKNOWLEDGED";
    default: return "RECOVERED";
    }
}

static labtwin_environment_event_state_t parse_state(const char *state)
{
    if (state && strcmp(state, "ACKNOWLEDGED") == 0)
        return LABTWIN_ENV_EVENT_ACKNOWLEDGED;
    if (state && strcmp(state, "RECOVERED") == 0)
        return LABTWIN_ENV_EVENT_RECOVERED;
    return LABTWIN_ENV_EVENT_ACTIVE;
}

static const char *sensor_name(env_sensor_t sensor)
{
    return sensor == SENSOR_TEMPERATURE ? "temperature" : "humidity";
}

static void response(char *out, size_t out_size, bool ok, const char *code,
                     const char *message, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    char *text;
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);
    if (data) cJSON_AddItemToObject(root, "data", data);
    else cJSON_AddObjectToObject(root, "data");
    text = cJSON_PrintUnformatted(root);
    if (out_size) snprintf(out, out_size, "%s", text ? text : "{\"ok\":false}");
    free(text);
    cJSON_Delete(root);
}

static int mkdir_checked(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -errno;
    chmod(path, 0700);
    return 0;
}

static int write_all(int fd, const char *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written <= 0) return -EIO;
        offset += (size_t)written;
    }
    return 0;
}

static int write_atomic(const char *path, const char *text)
{
    char temporary[192];
    int fd;
    int ret;
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    ret = write_all(fd, text, strlen(text));
#ifndef __NuttX__
    if (fsync(fd) < 0 && errno != ENOSYS) ret = -EIO;
#endif
    if (close(fd) < 0) ret = -EIO;
    if (!ret && rename(temporary, path) < 0) ret = -errno;
    if (!ret) chmod(path, 0600);
    return ret;
}

static env_telemetry_t *telemetry_at(size_t index)
{
    if (index >= g_telemetry_count)
        return NULL;
    return &g_telemetry[(g_telemetry_start + index) % ENV_TELEMETRY_MAX];
}

static void telemetry_append_memory_locked(const env_telemetry_t *point)
{
    size_t index;

    if (g_telemetry_count < ENV_TELEMETRY_MAX) {
        index = (g_telemetry_start + g_telemetry_count) % ENV_TELEMETRY_MAX;
        g_telemetry_count++;
    } else {
        index = g_telemetry_start;
        g_telemetry_start = (g_telemetry_start + 1) % ENV_TELEMETRY_MAX;
    }
    g_telemetry[index] = *point;
    if (point->seq > g_telemetry_seq)
        g_telemetry_seq = point->seq;
}

static cJSON *telemetry_json(const env_telemetry_t *point)
{
    cJSON *root = cJSON_CreateObject();

    if (!root)
        return NULL;
    cJSON_AddNumberToObject(root, "schema_version", ENV_SCHEMA_VERSION);
    cJSON_AddNumberToObject(root, "seq", (double)point->seq);
    cJSON_AddNumberToObject(root, "timestamp_epoch",
                            (double)point->timestamp_epoch);
    if (point->temperature_valid)
        cJSON_AddNumberToObject(root, "temperature_avg", point->temperature_avg);
    else
        cJSON_AddNullToObject(root, "temperature_avg");
    if (point->humidity_valid)
        cJSON_AddNumberToObject(root, "humidity_avg", point->humidity_avg);
    else
        cJSON_AddNullToObject(root, "humidity_avg");
    cJSON_AddNumberToObject(root, "valid_samples", point->valid_samples);
    cJSON_AddNumberToObject(root, "stale_samples", point->stale_samples);
    return root;
}

static bool telemetry_from_json(env_telemetry_t *point, const cJSON *root)
{
    const cJSON *item;

    if (!point || !root || !cJSON_IsObject(root) ||
        cJSON_GetNumberValue(cJSON_GetObjectItem(root, "schema_version")) !=
            ENV_SCHEMA_VERSION)
        return false;
    memset(point, 0, sizeof(*point));
    item = cJSON_GetObjectItem(root, "seq");
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0)
        return false;
    point->seq = (uint64_t)item->valuedouble;
    item = cJSON_GetObjectItem(root, "timestamp_epoch");
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0)
        return false;
    point->timestamp_epoch = (int64_t)item->valuedouble;
    item = cJSON_GetObjectItem(root, "temperature_avg");
    if (cJSON_IsNumber(item) && isfinite((float)item->valuedouble)) {
        point->temperature_avg = (float)item->valuedouble;
        point->temperature_valid = true;
    }
    item = cJSON_GetObjectItem(root, "humidity_avg");
    if (cJSON_IsNumber(item) && isfinite((float)item->valuedouble)) {
        point->humidity_avg = (float)item->valuedouble;
        point->humidity_valid = true;
    }
    item = cJSON_GetObjectItem(root, "valid_samples");
    point->valid_samples = cJSON_IsNumber(item) && item->valuedouble >= 0 ?
        (uint32_t)item->valuedouble : 0;
    item = cJSON_GetObjectItem(root, "stale_samples");
    point->stale_samples = cJSON_IsNumber(item) && item->valuedouble >= 0 ?
        (uint32_t)item->valuedouble : 0;
    return point->temperature_valid || point->humidity_valid ||
           point->stale_samples > 0;
}

static int write_telemetry_line(int fd, const env_telemetry_t *point)
{
    cJSON *root;
    char *text;
    int ret;

    root = telemetry_json(point);
    text = root ? cJSON_PrintUnformatted(root) : NULL;
    ret = (!text || fd < 0) ? -EIO : write_all(fd, text, strlen(text));
    if (!ret)
        ret = write_all(fd, "\n", 1);
    free(text);
    cJSON_Delete(root);
    return ret;
}

/* At the seven-day boundary, replace the completed five-minute file with
 * one point per UTC hour.  This keeps the current week detailed without
 * making the month-long dashboard or the NAND write budget unbounded. */
static int compact_recent_telemetry_locked(void)
{
    FILE *old_hourly;
    char line[ENV_TELEMETRY_LINE_SIZE];
    size_t active_start;
    size_t index;
    unsigned int new_hours = 0;
    unsigned int skip_hours;
    int fd = -1;
    int ret = 0;

    if (g_telemetry_active_count < ENV_TELEMETRY_RECENT_MAX)
        return 0;
    active_start = g_telemetry_count - g_telemetry_active_count;
    for (index = active_start; index < g_telemetry_count; index++) {
        env_telemetry_t *point = telemetry_at(index);
        env_telemetry_t *previous = index == active_start ? NULL : telemetry_at(index - 1);
        if (point && (!previous || point->timestamp_epoch / ENV_TELEMETRY_HOUR_SECONDS !=
                       previous->timestamp_epoch / ENV_TELEMETRY_HOUR_SECONDS))
            new_hours++;
    }
    skip_hours = g_telemetry_hourly_count + new_hours > ENV_TELEMETRY_HOURLY_MAX ?
        g_telemetry_hourly_count + new_hours - ENV_TELEMETRY_HOURLY_MAX : 0;
    fd = open(ENV_TELEMETRY_HOURLY_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -errno;
    old_hourly = fopen(ENV_TELEMETRY_HOURLY, "r");
    while (!ret && old_hourly && fgets(line, sizeof(line), old_hourly)) {
        if (skip_hours) {
            skip_hours--;
            continue;
        }
        ret = write_all(fd, line, strlen(line));
    }
    if (old_hourly)
        fclose(old_hourly);

    for (index = active_start; !ret && index < g_telemetry_count;) {
        env_telemetry_t aggregate;
        int64_t hour;
        uint32_t temperature_count = 0;
        uint32_t humidity_count = 0;

        memset(&aggregate, 0, sizeof(aggregate));
        hour = telemetry_at(index)->timestamp_epoch / ENV_TELEMETRY_HOUR_SECONDS;
        aggregate.timestamp_epoch = hour * ENV_TELEMETRY_HOUR_SECONDS;
        while (index < g_telemetry_count) {
            env_telemetry_t *point = telemetry_at(index);
            if (!point || point->timestamp_epoch / ENV_TELEMETRY_HOUR_SECONDS != hour)
                break;
            aggregate.seq = point->seq;
            if (point->temperature_valid) {
                aggregate.temperature_avg += point->temperature_avg;
                temperature_count++;
            }
            if (point->humidity_valid) {
                aggregate.humidity_avg += point->humidity_avg;
                humidity_count++;
            }
            aggregate.valid_samples += point->valid_samples;
            aggregate.stale_samples += point->stale_samples;
            index++;
        }
        aggregate.temperature_valid = temperature_count > 0;
        aggregate.humidity_valid = humidity_count > 0;
        if (aggregate.temperature_valid)
            aggregate.temperature_avg /= temperature_count;
        if (aggregate.humidity_valid)
            aggregate.humidity_avg /= humidity_count;
        ret = write_telemetry_line(fd, &aggregate);
    }
    if (close(fd) < 0 && !ret)
        ret = -EIO;
    if (ret) {
        unlink(ENV_TELEMETRY_HOURLY_TMP);
        return ret;
    }
    chmod(ENV_TELEMETRY_HOURLY_TMP, 0600);
    if (rename(ENV_TELEMETRY_HOURLY_TMP, ENV_TELEMETRY_HOURLY) < 0)
        return -errno;
    fd = open(ENV_TELEMETRY, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -errno;
    if (close(fd) < 0)
        return -EIO;
    chmod(ENV_TELEMETRY, 0600);
    return 0;
}

static int append_telemetry_locked(const env_telemetry_t *point)
{
    int fd;
    int ret;

    if (g_telemetry_active_count >= ENV_TELEMETRY_RECENT_MAX) {
        ret = compact_recent_telemetry_locked();
        if (ret)
            return ret;
        /* The caller will add the first point of the new detailed week. */
        load_telemetry_locked();
    }
    fd = open(ENV_TELEMETRY, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return -errno;
    ret = write_telemetry_line(fd, point);
    if (close(fd) < 0 && !ret)
        ret = -EIO;
    if (!ret) {
        chmod(ENV_TELEMETRY, 0600);
        g_telemetry_active_count++;
    }
    return ret;
}

static void telemetry_collect_locked(uint64_t now_ms)
{
    env_telemetry_t point;
    bool temperature_valid;
    bool humidity_valid;

    if (!labtwin_clock_trusted()) {
        g_telemetry_window_until_ms = 0;
        g_telemetry_temperature_sum = 0.0f;
        g_telemetry_humidity_sum = 0.0f;
        g_telemetry_temperature_count = 0;
        g_telemetry_humidity_count = 0;
        g_telemetry_stale_count = 0;
        return;
    }
    if (!g_telemetry_window_until_ms)
        g_telemetry_window_until_ms = now_ms + ENV_TELEMETRY_WINDOW_MS;
    temperature_valid = g_samples[SENSOR_TEMPERATURE].valid &&
        now_ms >= g_samples[SENSOR_TEMPERATURE].sampled_ms &&
        now_ms - g_samples[SENSOR_TEMPERATURE].sampled_ms <= ENV_STALE_MS;
    humidity_valid = g_samples[SENSOR_HUMIDITY].valid &&
        now_ms >= g_samples[SENSOR_HUMIDITY].sampled_ms &&
        now_ms - g_samples[SENSOR_HUMIDITY].sampled_ms <= ENV_STALE_MS;
    if (temperature_valid) {
        g_telemetry_temperature_sum += g_samples[SENSOR_TEMPERATURE].value;
        g_telemetry_temperature_count++;
    }
    if (humidity_valid) {
        g_telemetry_humidity_sum += g_samples[SENSOR_HUMIDITY].value;
        g_telemetry_humidity_count++;
    }
    if (!temperature_valid || !humidity_valid)
        g_telemetry_stale_count++;
    if (now_ms < g_telemetry_window_until_ms)
        return;

    memset(&point, 0, sizeof(point));
    point.seq = ++g_telemetry_seq;
    point.timestamp_epoch = wall_epoch();
    point.temperature_valid = g_telemetry_temperature_count > 0;
    point.humidity_valid = g_telemetry_humidity_count > 0;
    point.temperature_avg = point.temperature_valid ?
        g_telemetry_temperature_sum / g_telemetry_temperature_count : 0.0f;
    point.humidity_avg = point.humidity_valid ?
        g_telemetry_humidity_sum / g_telemetry_humidity_count : 0.0f;
    point.valid_samples = g_telemetry_temperature_count +
                          g_telemetry_humidity_count;
    point.stale_samples = g_telemetry_stale_count;
    if (point.temperature_valid || point.humidity_valid || point.stale_samples) {
        if (append_telemetry_locked(&point) != 0)
            g_storage_error = true;
        else
            telemetry_append_memory_locked(&point);
    }
    g_telemetry_window_until_ms = now_ms + ENV_TELEMETRY_WINDOW_MS;
    g_telemetry_temperature_sum = 0.0f;
    g_telemetry_humidity_sum = 0.0f;
    g_telemetry_temperature_count = 0;
    g_telemetry_humidity_count = 0;
    g_telemetry_stale_count = 0;
}

static cJSON *event_json(const labtwin_environment_event_view_t *event)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", event->event_id);
    cJSON_AddStringToObject(root, "rule_id", event->rule_id);
    cJSON_AddStringToObject(root, "severity", "WARNING");
    cJSON_AddStringToObject(root, "state", state_name(event->state));
    cJSON_AddStringToObject(root, "sensor", event->sensor);
    cJSON_AddNumberToObject(root, "measured_value", event->measured_value);
    cJSON_AddNumberToObject(root, "trigger_threshold", event->trigger_threshold);
    cJSON_AddNumberToObject(root, "clear_threshold", event->clear_threshold);
    if (event->created_epoch) cJSON_AddNumberToObject(root, "created_epoch", (double)event->created_epoch);
    else cJSON_AddNullToObject(root, "created_epoch");
    if (event->acknowledged_epoch) cJSON_AddNumberToObject(root, "acknowledged_epoch", (double)event->acknowledged_epoch);
    else cJSON_AddNullToObject(root, "acknowledged_epoch");
    if (event->recovered_epoch) cJSON_AddNumberToObject(root, "recovered_epoch", (double)event->recovered_epoch);
    else cJSON_AddNullToObject(root, "recovered_epoch");
    if (event->ack_source[0]) cJSON_AddStringToObject(root, "ack_source", event->ack_source);
    else cJSON_AddNullToObject(root, "ack_source");
    if (event->experiment_id[0]) cJSON_AddStringToObject(root, "experiment_id", event->experiment_id);
    else cJSON_AddNullToObject(root, "experiment_id");
    return root;
}

static bool event_from_json(labtwin_environment_event_view_t *event,
                            const cJSON *root)
{
    const cJSON *item;
    const char *text;
    memset(event, 0, sizeof(*event));
    item = cJSON_GetObjectItem(root, "event_id");
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    snprintf(event->event_id, sizeof(event->event_id), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "rule_id");
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    snprintf(event->rule_id, sizeof(event->rule_id), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "sensor");
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    snprintf(event->sensor, sizeof(event->sensor), "%s", item->valuestring);
    text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
    event->state = parse_state(text);
    event->measured_value = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "measured_value"));
    event->trigger_threshold = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "trigger_threshold"));
    event->clear_threshold = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "clear_threshold"));
    event->created_epoch = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "created_epoch"));
    event->acknowledged_epoch = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "acknowledged_epoch"));
    event->recovered_epoch = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "recovered_epoch"));
    item = cJSON_GetObjectItem(root, "ack_source");
    if (cJSON_IsString(item)) snprintf(event->ack_source, sizeof(event->ack_source), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "experiment_id");
    if (cJSON_IsString(item)) snprintf(event->experiment_id, sizeof(event->experiment_id), "%s", item->valuestring);
    return true;
}

#ifdef LABTWIN_ENV_HOST_TEST
static cJSON *config_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *rules = cJSON_AddArrayToObject(root, "rules");
    int i;
    cJSON_AddNumberToObject(root, "schema_version", ENV_SCHEMA_VERSION);
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "rule_id", g_rules[i].id);
        cJSON_AddNumberToObject(item, "trigger", g_rules[i].trigger);
        cJSON_AddNumberToObject(item, "clear", g_rules[i].clear);
        cJSON_AddItemToArray(rules, item);
    }
    return root;
}
#endif

static int persist_config_locked(void)
{
#ifdef LABTWIN_ENV_HOST_TEST
    cJSON *root = config_json();
    char *text = cJSON_PrintUnformatted(root);
    int ret = text ? write_atomic(ENV_CONFIG, text) : -ENOMEM;
    free(text);
    cJSON_Delete(root);
    return ret;
#else
    char value[PROPERTY_VALUE_MAX];
    int length;

    /* Runtime writes to /data (YAFFS) can wedge the R528 NAND worker and
     * stop HTTP, ADB, sensors, and the display together.  Keep this compact
     * record in the named MTD configuration area through KVDB/NVS instead. */
    length = snprintf(value, sizeof(value),
                      "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                      ENV_SCHEMA_VERSION,
                      g_rules[0].trigger, g_rules[0].clear,
                      g_rules[1].trigger, g_rules[1].clear,
                      g_rules[2].trigger, g_rules[2].clear,
                      g_rules[3].trigger, g_rules[3].clear);
    if (length <= 0 || length >= (int)sizeof(value))
        return -E2BIG;
    return property_set(ENV_PROPERTY_KEY, value) == 0 ? 0 : -EIO;
#endif
}

static int rule_index(const char *id)
{
    int i;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++)
        if (id && strcmp(g_rules[i].id, id) == 0) return i;
    return -1;
}

static bool valid_rule_values(const env_rule_t *rule, float trigger,
                              float clear)
{
    if (!isfinite(trigger) || !isfinite(clear)) return false;
    if (rule->sensor == SENSOR_HUMIDITY &&
        (trigger < 0 || trigger > 100 || clear < 0 || clear > 100)) return false;
    if (rule->sensor == SENSOR_TEMPERATURE &&
        (trigger < -40 || trigger > 125 || clear < -40 || clear > 125)) return false;
    return rule->direction == RULE_HIGH ? trigger > clear : trigger < clear;
}

static void defaults_locked(void)
{
    static const float trigger[] = {30.0f, 10.0f, 70.0f, 20.0f};
    static const float clear[] = {28.0f, 12.0f, 65.0f, 25.0f};
    int i;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        g_rules[i].trigger = trigger[i];
        g_rules[i].clear = clear[i];
        g_rules[i].pending_count = 0;
        g_rules[i].clear_count = 0;
    }
}

static int read_file(const char *path, char **output)
{
    FILE *file = fopen(path, "r");
    long size;
    char *buffer;
    if (!file) return -errno;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(file); return -EINVAL; }
    buffer = malloc((size_t)size + 1);
    if (!buffer) { fclose(file); return -ENOMEM; }
    buffer[fread(buffer, 1, (size_t)size, file)] = 0;
    fclose(file);
    *output = buffer;
    return 0;
}

static void load_telemetry_file_locked(const char *path, bool active_file)
{
    FILE *file = fopen(path, "r");
    char line[ENV_TELEMETRY_LINE_SIZE];
    bool corrupt = false;

    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        size_t length = strlen(line);
        cJSON *root;
        env_telemetry_t point;

        if (!length || line[length - 1] != '\n')
            break;
        root = cJSON_Parse(line);
        if (!root || !telemetry_from_json(&point, root)) {
            corrupt = true;
            cJSON_Delete(root);
            break;
        }
        if (point.seq > g_telemetry_seq) {
            telemetry_append_memory_locked(&point);
            if (active_file)
                g_telemetry_active_count++;
            else
                g_telemetry_hourly_count++;
        }
        cJSON_Delete(root);
    }
    fclose(file);
    if (corrupt)
        g_storage_error = true;
}

static void load_telemetry_locked(void)
{
    g_telemetry_start = 0;
    g_telemetry_count = 0;
    g_telemetry_seq = 0;
    g_telemetry_active_count = 0;
    g_telemetry_hourly_count = 0;
    load_telemetry_file_locked(ENV_TELEMETRY_HOURLY, false);
    load_telemetry_file_locked(ENV_TELEMETRY, true);
    if (g_telemetry_active_count > ENV_TELEMETRY_RECENT_MAX ||
        g_telemetry_hourly_count > ENV_TELEMETRY_HOURLY_MAX) {
        g_storage_error = true;
    }
}

#ifndef LABTWIN_ENV_HOST_TEST
static int load_property_config_locked(const char *key)
{
    char value[PROPERTY_VALUE_MAX] = { 0 };
    char extra;
    int version;
    float trigger[LABTWIN_ENV_RULE_COUNT];
    float clear[LABTWIN_ENV_RULE_COUNT];
    int count;
    int i;

    if (!key || property_get_with_err(key, value) <= 0)
        return -ENOENT;
    count = sscanf(value, "%d,%f,%f,%f,%f,%f,%f,%f,%f%c",
                   &version,
                   &trigger[0], &clear[0], &trigger[1], &clear[1],
                   &trigger[2], &clear[2], &trigger[3], &clear[3],
                   &extra);
    if (count != 9 || version != ENV_SCHEMA_VERSION)
        return -EINVAL;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++)
        if (!valid_rule_values(&g_rules[i], trigger[i], clear[i]))
            return -EINVAL;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        g_rules[i].trigger = trigger[i];
        g_rules[i].clear = clear[i];
    }
    return 0;
}
#endif

static int load_config_locked(void)
{
    char *text = NULL;
    cJSON *root;
    cJSON *rules;
    cJSON *item;
    int seen = 0;
    int ret;
    defaults_locked();
#ifdef LABTWIN_ENV_HOST_TEST
    ret = read_file(ENV_CONFIG, &text);
#else
    ret = load_property_config_locked(ENV_PROPERTY_KEY);
    if (ret == 0)
        return 0;
    if (ret != -ENOENT)
        goto fallback;

    ret = load_property_config_locked(ENV_LEGACY_PROPERTY_KEY);
    if (ret == 0) {
        if (persist_config_locked() != 0)
            return -EIO;
        syslog(LOG_INFO, "[%s] migrated legacy property configuration\n", TAG);
        return 0;
    }
    if (ret != -ENOENT)
        goto fallback;

    /* One-time compatibility read from the former YAFFS JSON backend. */
    char stored[1024];
    ret = claw_config_get(ENV_CONFIG_KEY, stored, sizeof(stored));
    if (ret != OK)
        ret = claw_config_get(ENV_LEGACY_CONFIG_KEY, stored, sizeof(stored));
    if (ret == OK) {
        text = strdup(stored);
        if (!text) ret = -ENOMEM;
    }
#endif
    if (ret) return persist_config_locked();
    root = cJSON_Parse(text);
    free(text);
    if (!root || cJSON_GetNumberValue(cJSON_GetObjectItem(root, "schema_version")) != ENV_SCHEMA_VERSION ||
        !cJSON_IsArray(rules = cJSON_GetObjectItem(root, "rules"))) goto invalid;
    cJSON_ArrayForEach(item, rules) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "rule_id"));
        float trigger = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "trigger"));
        float clear = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "clear"));
        int index = rule_index(id);
        if (index < 0 || !valid_rule_values(&g_rules[index], trigger, clear)) goto invalid;
        g_rules[index].trigger = trigger;
        g_rules[index].clear = clear;
        seen++;
    }
    cJSON_Delete(root);
    if (seen != LABTWIN_ENV_RULE_COUNT) goto fallback;
#ifndef LABTWIN_ENV_HOST_TEST
    /* Migrate a valid legacy value once; future updates never rewrite YAFFS. */
    if (persist_config_locked() != 0)
        return -EIO;
#endif
    return 0;
invalid:
    cJSON_Delete(root);
fallback:
    defaults_locked();
    g_config_fallback = true;
    syslog(LOG_ERR, "[%s] invalid config; defaults restored\n", TAG);
    return persist_config_locked();
}

static int find_event(const char *event_id)
{
    int i;
    for (i = 0; i < ENV_MAX_EVENTS; i++)
        if (g_events[i].used && strcmp(g_events[i].view.event_id, event_id) == 0)
            return i;
    return -1;
}

static int find_active_rule(const char *rule_id)
{
    int i;
    for (i = 0; i < ENV_MAX_EVENTS; i++)
        if (g_events[i].used &&
            g_events[i].view.state != LABTWIN_ENV_EVENT_RECOVERED &&
            strcmp(g_events[i].view.rule_id, rule_id) == 0) return i;
    return -1;
}

static int allocate_event(void)
{
    int i;
    for (i = 0; i < ENV_MAX_EVENTS; i++) if (!g_events[i].used) return i;
    for (i = 0; i < ENV_MAX_EVENTS; i++)
        if (g_events[i].view.state == LABTWIN_ENV_EVENT_RECOVERED) return i;
    return -1;
}

static int persist_snapshot_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    cJSON *cooldowns = cJSON_AddObjectToObject(root, "cooldown_until_epoch");
    char *text;
    int i;
    int ret;
    cJSON_AddNumberToObject(root, "schema_version", ENV_SCHEMA_VERSION);
    cJSON_AddNumberToObject(root, "last_event_seq", (double)g_event_seq);
    cJSON_AddBoolToObject(root, "storage_error", g_storage_error);
    for (i = 0; i < ENV_MAX_EVENTS; i++)
        if (g_events[i].used) cJSON_AddItemToArray(events, event_json(&g_events[i].view));
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++)
        cJSON_AddNumberToObject(cooldowns, g_rules[i].id,
                                (double)g_rules[i].cooldown_until_epoch);
    text = cJSON_PrintUnformatted(root);
    ret = text ? write_atomic(ENV_SNAPSHOT, text) : -ENOMEM;
    free(text);
    cJSON_Delete(root);
    return ret;
}

static int append_transition_locked(const char *action,
                                    const labtwin_environment_event_view_t *event)
{
    cJSON *root = cJSON_CreateObject();
    char *text;
    int fd;
    int ret;
    cJSON_AddNumberToObject(root, "schema_version", ENV_SCHEMA_VERSION);
    cJSON_AddNumberToObject(root, "seq", (double)g_event_seq);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddItemToObject(root, "payload", event_json(event));
    text = cJSON_PrintUnformatted(root);
    fd = open(ENV_EVENTS, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0 && lseek(fd, 0, SEEK_END) < 0) {
        close(fd);
        fd = -1;
    }
    ret = (fd < 0 || !text) ? -EIO : write_all(fd, text, strlen(text));
    if (!ret) ret = write_all(fd, "\n", 1);
    if (fd >= 0) {
#ifndef __NuttX__
        if (fsync(fd) < 0 && errno != ENOSYS) ret = -EIO;
#endif
        close(fd);
        chmod(ENV_EVENTS, 0600);
    }
    free(text);
    cJSON_Delete(root);
    if (!ret) ret = persist_snapshot_locked();
    if (ret) g_storage_error = true;
    return ret;
}

static void apply_replayed_event(uint64_t seq, const cJSON *payload)
{
    labtwin_environment_event_view_t event;
    int index;
    if (seq <= g_event_seq || !event_from_json(&event, payload)) return;
    index = find_event(event.event_id);
    if (index < 0) index = allocate_event();
    if (index >= 0) {
        g_events[index].used = true;
        g_events[index].view = event;
        g_event_seq = seq;
    }
}

static void load_state_locked(void)
{
    char *text = NULL;
    cJSON *root;
    cJSON *events;
    cJSON *item;
    int i;
    if (read_file(ENV_SNAPSHOT, &text) == 0) {
        root = cJSON_Parse(text);
        free(text);
        if (root && cJSON_GetNumberValue(cJSON_GetObjectItem(root, "schema_version")) == ENV_SCHEMA_VERSION) {
            g_event_seq = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "last_event_seq"));
            g_storage_error = cJSON_IsTrue(cJSON_GetObjectItem(root, "storage_error"));
            events = cJSON_GetObjectItem(root, "events");
            if (cJSON_IsArray(events)) cJSON_ArrayForEach(item, events) {
                int index = allocate_event();
                if (index >= 0 && event_from_json(&g_events[index].view, item))
                    g_events[index].used = true;
            }
            item = cJSON_GetObjectItem(root, "cooldown_until_epoch");
            if (cJSON_IsObject(item)) for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++)
                g_rules[i].cooldown_until_epoch = (int64_t)cJSON_GetNumberValue(
                    cJSON_GetObjectItem(item, g_rules[i].id));
        } else {
            g_storage_error = true;
        }
        cJSON_Delete(root);
    }
    {
        FILE *file = fopen(ENV_EVENTS, "r");
        if (file) {
            char line[ENV_LINE_SIZE];
            bool corrupt = false;
            while (fgets(line, sizeof(line), file)) {
                size_t length = strlen(line);
                cJSON *entry;
                if (!length || line[length - 1] != '\n') break;
                entry = cJSON_Parse(line);
                if (!entry) { corrupt = true; break; }
                apply_replayed_event((uint64_t)cJSON_GetNumberValue(
                    cJSON_GetObjectItem(entry, "seq")),
                    cJSON_GetObjectItem(entry, "payload"));
                cJSON_Delete(entry);
            }
            fclose(file);
            if (corrupt) g_storage_error = true;
        }
    }
    load_telemetry_locked();
}

static bool trigger_matches(const env_rule_t *rule, float value)
{
    return rule->direction == RULE_HIGH ? value >= rule->trigger : value <= rule->trigger;
}

static bool clear_matches(const env_rule_t *rule, float value)
{
    return rule->direction == RULE_HIGH ? value <= rule->clear : value >= rule->clear;
}

static void create_event_locked(env_rule_t *rule, float value)
{
    int index = allocate_event();
    labtwin_environment_event_view_t *event;
    char experiment[32] = {0};
    if (index < 0) { g_storage_error = true; return; }
    event = &g_events[index].view;
    memset(event, 0, sizeof(*event));
    g_event_seq++;
    snprintf(event->event_id, sizeof(event->event_id), "ENV-%08lx",
             (unsigned long)g_event_seq);
    snprintf(event->rule_id, sizeof(event->rule_id), "%s", rule->id);
    snprintf(event->sensor, sizeof(event->sensor), "%s", sensor_name(rule->sensor));
    if (labtwin_environment_resolve_experiment(g_foreground, experiment,
                                               sizeof(experiment)) == 0)
        snprintf(event->experiment_id, sizeof(event->experiment_id), "%s", experiment);
    event->state = LABTWIN_ENV_EVENT_ACTIVE;
    event->measured_value = value;
    event->trigger_threshold = rule->trigger;
    event->clear_threshold = rule->clear;
    event->created_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    g_events[index].used = true;
    append_transition_locked("CREATED", event);
}

static void recover_event_locked(env_rule_t *rule, int index, float value,
                                 uint64_t now_ms)
{
    labtwin_environment_event_view_t *event = &g_events[index].view;
    event->state = LABTWIN_ENV_EVENT_RECOVERED;
    event->measured_value = value;
    event->recovered_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    g_event_seq++;
    rule->cooldown_until_ms = now_ms + ENV_COOLDOWN_MS;
    rule->cooldown_until_epoch = labtwin_clock_trusted() ? wall_epoch() + 60 : 0;
    append_transition_locked("RECOVERED", event);
    g_notice_index = index;
    g_notice_until_ms = now_ms + ENV_NOTICE_MS;
}

static void process_locked(uint64_t now_ms)
{
    int i;
    int64_t epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        env_rule_t *rule = &g_rules[i];
        env_sample_t *sample = &g_samples[rule->sensor];
        int active;
        if (!sample->valid || now_ms < sample->sampled_ms ||
            now_ms - sample->sampled_ms > ENV_STALE_MS) {
            rule->pending_count = 0;
            rule->clear_count = 0;
            rule->last_sample_seq = sample->seq;
            continue;
        }
        if (rule->last_sample_seq == sample->seq) continue;
        rule->last_sample_seq = sample->seq;
        active = find_active_rule(rule->id);
        if (active >= 0) {
            rule->pending_count = 0;
            if (clear_matches(rule, sample->value)) {
                if (++rule->clear_count >= ENV_CLEAR_SAMPLES) {
                    recover_event_locked(rule, active, sample->value, now_ms);
                    rule->clear_count = 0;
                }
            } else {
                rule->clear_count = 0;
            }
            continue;
        }
        rule->clear_count = 0;
        if (now_ms < rule->cooldown_until_ms ||
            (epoch && epoch < rule->cooldown_until_epoch)) {
            rule->pending_count = 0;
            continue;
        }
        if (trigger_matches(rule, sample->value)) {
            if (++rule->pending_count >= ENV_TRIGGER_SAMPLES) {
                create_event_locked(rule, sample->value);
                rule->pending_count = 0;
            }
        } else {
            rule->pending_count = 0;
        }
    }
}

#ifndef LABTWIN_ENV_HOST_TEST
static void *environment_thread(void *arg)
{
    (void)arg;
    while (g_running) {
        pthread_mutex_lock(&g_env_lock);
        if (g_injected[SENSOR_TEMPERATURE]) {
            g_samples[SENSOR_TEMPERATURE].valid = true;
            g_samples[SENSOR_TEMPERATURE].value = g_injected_value[SENSOR_TEMPERATURE];
            g_samples[SENSOR_TEMPERATURE].sampled_ms = monotonic_ms();
            g_samples[SENSOR_TEMPERATURE].seq++;
        }
        if (g_injected[SENSOR_HUMIDITY]) {
            g_samples[SENSOR_HUMIDITY].valid = true;
            g_samples[SENSOR_HUMIDITY].value = g_injected_value[SENSOR_HUMIDITY];
            g_samples[SENSOR_HUMIDITY].sampled_ms = monotonic_ms();
            g_samples[SENSOR_HUMIDITY].seq++;
        }
        process_locked(monotonic_ms());
        telemetry_collect_locked(monotonic_ms());
        pthread_mutex_unlock(&g_env_lock);
        sleep(1);
    }
    return NULL;
}
#endif

int labtwin_environment_init(void)
{
    int ret = 0;
    pthread_mutex_lock(&g_env_lock);
    if (g_initialized) { pthread_mutex_unlock(&g_env_lock); return 0; }
    mkdir_checked(LABTWIN_ROOT);
    if (mkdir_checked(ENV_ROOT) != 0) ret = -EIO;
    if (!ret && load_config_locked() != 0) ret = -EIO;
    if (!ret) load_state_locked();
#ifndef LABTWIN_ENV_HOST_TEST
    if (!ret) {
        g_running = true;
        if (pthread_create(&g_env_thread, NULL, environment_thread, NULL) != 0) {
            g_running = false;
            ret = -EIO;
        } else {
            pthread_detach(g_env_thread);
        }
    }
#endif
    if (!ret) g_initialized = true;
    pthread_mutex_unlock(&g_env_lock);
    if (!ret) syslog(LOG_INFO, "[%s] environment service ready\n", TAG);
    return ret;
}

void labtwin_environment_submit(bool temperature_valid, float temperature_c,
                                bool humidity_valid, float humidity_percent)
{
    uint64_t now = monotonic_ms();
    pthread_mutex_lock(&g_env_lock);
    if (temperature_valid) {
        env_sample_t *sample = &g_samples[SENSOR_TEMPERATURE];
        sample->seq++;
        if (isfinite(temperature_c) && temperature_c >= -40.0f && temperature_c <= 125.0f) {
            sample->valid = true; sample->value = temperature_c;
            sample->sampled_ms = now;
        } else { sample->valid = false; sample->failures++; }
    }
    if (humidity_valid) {
        env_sample_t *sample = &g_samples[SENSOR_HUMIDITY];
        sample->seq++;
        if (isfinite(humidity_percent) && humidity_percent >= 0.0f && humidity_percent <= 100.0f) {
            sample->valid = true; sample->value = humidity_percent;
            sample->sampled_ms = now;
        } else { sample->valid = false; sample->failures++; }
    }
    pthread_mutex_unlock(&g_env_lock);
}

void labtwin_environment_set_foreground(const char *experiment_id)
{
    pthread_mutex_lock(&g_env_lock);
    snprintf(g_foreground, sizeof(g_foreground), "%s", experiment_id ? experiment_id : "");
    pthread_mutex_unlock(&g_env_lock);
}

static cJSON *rules_json_locked(void)
{
    cJSON *array = cJSON_CreateArray();
    int i;
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "rule_id", g_rules[i].id);
        cJSON_AddStringToObject(item, "sensor", sensor_name(g_rules[i].sensor));
        cJSON_AddNumberToObject(item, "trigger", g_rules[i].trigger);
        cJSON_AddNumberToObject(item, "clear", g_rules[i].clear);
        cJSON_AddNumberToObject(item, "pending_samples", g_rules[i].pending_count);
        cJSON_AddNumberToObject(item, "clear_samples", g_rules[i].clear_count);
        cJSON_AddItemToArray(array, item);
    }
    return array;
}

int labtwin_environment_status_json(char *out, size_t out_size)
{
    cJSON *data = cJSON_CreateObject();
    uint64_t now = monotonic_ms();
    int i;
    int active = 0;
    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < ENV_MAX_EVENTS; i++)
        if (g_events[i].used && g_events[i].view.state != LABTWIN_ENV_EVENT_RECOVERED) active++;
    cJSON_AddBoolToObject(data, "storage_error", g_storage_error);
    cJSON_AddBoolToObject(data, "config_fallback", g_config_fallback);
    cJSON_AddNumberToObject(data, "active_events", active);
    cJSON_AddBoolToObject(data, "temperature_stale", !g_samples[SENSOR_TEMPERATURE].valid ||
        now - g_samples[SENSOR_TEMPERATURE].sampled_ms > ENV_STALE_MS);
    cJSON_AddBoolToObject(data, "humidity_stale", !g_samples[SENSOR_HUMIDITY].valid ||
        now - g_samples[SENSOR_HUMIDITY].sampled_ms > ENV_STALE_MS);
    cJSON_AddNumberToObject(data, "temperature_failures", g_samples[SENSOR_TEMPERATURE].failures);
    cJSON_AddNumberToObject(data, "humidity_failures", g_samples[SENSOR_HUMIDITY].failures);
    cJSON_AddItemToObject(data, "rules", rules_json_locked());
    response(out, out_size, true, "OK", "environment status", data);
    pthread_mutex_unlock(&g_env_lock);
    return 0;
}

int labtwin_environment_events_json(const char *input, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(data, "events");
    const char *filter = NULL;
    int i;
    if (!root || !cJSON_IsObject(root)) goto invalid;
    if (cJSON_GetObjectItem(root, "state")) {
        filter = cJSON_GetStringValue(cJSON_GetObjectItem(root, "state"));
        if (!filter || (strcmp(filter, "active") && strcmp(filter, "all"))) goto invalid;
    }
    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < ENV_MAX_EVENTS; i++) {
        if (!g_events[i].used) continue;
        if (filter && strcmp(filter, "active") == 0 &&
            g_events[i].view.state == LABTWIN_ENV_EVENT_RECOVERED) continue;
        cJSON_AddItemToArray(array, event_json(&g_events[i].view));
    }
    response(out, out_size, true, "OK", "environment events", data);
    pthread_mutex_unlock(&g_env_lock);
    cJSON_Delete(root);
    return 0;
invalid:
    cJSON_Delete(root); cJSON_Delete(data);
    response(out, out_size, false, "INVALID_ARGUMENT", "invalid event filter", NULL);
    return -EINVAL;
}

int labtwin_environment_dashboard_json(char *out, size_t out_size)
{
    cJSON *root;
    cJSON *data;
    cJSON *events;
    cJSON *telemetry;
    char *text;
    size_t length;
    size_t index;
    size_t stride;
    uint64_t now = monotonic_ms();
    int i;

    if (!out || out_size == 0)
        return -EINVAL;

    root = cJSON_CreateObject();
    data = cJSON_CreateObject();
    events = cJSON_AddArrayToObject(data, "events");
    telemetry = cJSON_AddArrayToObject(data, "telemetry");
    if (!root || !data || !events || !telemetry) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return -ENOMEM;
    }

    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < ENV_MAX_EVENTS; i++) {
        if (g_events[i].used)
            cJSON_AddItemToArray(events, event_json(&g_events[i].view));
    }
    stride = (g_telemetry_count + ENV_TELEMETRY_EMIT_MAX - 1) /
             ENV_TELEMETRY_EMIT_MAX;
    if (stride == 0)
        stride = 1;
    for (index = 0; index < g_telemetry_count; index += stride) {
        env_telemetry_t *point = telemetry_at(index);
        if (point)
            cJSON_AddItemToArray(telemetry, telemetry_json(point));
    }
    if (g_telemetry_count > 1 &&
        (g_telemetry_count - 1) % stride != 0) {
        env_telemetry_t *point = telemetry_at(g_telemetry_count - 1);
        if (point)
            cJSON_AddItemToArray(telemetry, telemetry_json(point));
    }
    cJSON_AddNumberToObject(data, "last_event_seq", (double)g_event_seq);
    cJSON_AddNumberToObject(data, "last_telemetry_seq",
                            (double)g_telemetry_seq);
    cJSON_AddBoolToObject(data, "storage_error", g_storage_error);
    cJSON_AddBoolToObject(data, "sensor_stale",
        !g_samples[SENSOR_TEMPERATURE].valid ||
        !g_samples[SENSOR_HUMIDITY].valid ||
        now - g_samples[SENSOR_TEMPERATURE].sampled_ms > ENV_STALE_MS ||
        now - g_samples[SENSOR_HUMIDITY].sampled_ms > ENV_STALE_MS);
    pthread_mutex_unlock(&g_env_lock);

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "code", "OK");
    cJSON_AddStringToObject(root, "message", "environment dashboard snapshot");
    cJSON_AddItemToObject(root, "data", data);
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
        return -ENOMEM;
    length = strlen(text);
    if (length >= out_size) {
        free(text);
        out[0] = '\0';
        return -ENOSPC;
    }
    memcpy(out, text, length + 1);
    free(text);
    return 0;
}

int labtwin_environment_get_json(const char *input, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    const char *id;
    int index;
    if (!root || !(id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event_id")))) goto invalid;
    pthread_mutex_lock(&g_env_lock);
    index = find_event(id);
    if (index < 0) {
        response(out, out_size, false, "NOT_FOUND", "environment event not found", NULL);
        pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return -ENOENT;
    }
    response(out, out_size, true, "OK", "environment event", event_json(&g_events[index].view));
    pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return 0;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "event_id is required", NULL); return -EINVAL;
}

int labtwin_environment_ack_json(const char *input, char *out, size_t out_size,
                                 const char *source)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    const char *id;
    int index;
    labtwin_environment_event_view_t *event;
    if (!root || !(id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event_id")))) goto invalid;
    pthread_mutex_lock(&g_env_lock);
    index = find_event(id);
    if (index < 0) {
        response(out, out_size, false, "NOT_FOUND", "environment event not found", NULL);
        pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return -ENOENT;
    }
    event = &g_events[index].view;
    if (event->state == LABTWIN_ENV_EVENT_ACKNOWLEDGED) {
        response(out, out_size, true, "ALREADY_APPLIED", "event already acknowledged", event_json(event));
        pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return 0;
    }
    if (event->state == LABTWIN_ENV_EVENT_RECOVERED) {
        response(out, out_size, false, "INVALID_STATE", "recovered event cannot be acknowledged", event_json(event));
        pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return -EINVAL;
    }
    event->state = LABTWIN_ENV_EVENT_ACKNOWLEDGED;
    event->acknowledged_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    snprintf(event->ack_source, sizeof(event->ack_source), "%s", source ? source : "system");
    g_event_seq++;
    append_transition_locked("ACKNOWLEDGED", event);
    response(out, out_size, true, g_storage_error ? "STORAGE_ERROR" : "OK",
             g_storage_error ? "acknowledged in memory; persistence failed" : "event acknowledged",
             event_json(event));
    pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root);
    return g_storage_error ? -EIO : 0;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "event_id is required", NULL); return -EINVAL;
}

int labtwin_environment_rule_show_json(char *out, size_t out_size)
{
    cJSON *data = cJSON_CreateObject();
    pthread_mutex_lock(&g_env_lock);
    cJSON_AddNumberToObject(data, "schema_version", ENV_SCHEMA_VERSION);
    cJSON_AddItemToObject(data, "rules", rules_json_locked());
    response(out, out_size, true, "OK", "environment rules", data);
    pthread_mutex_unlock(&g_env_lock);
    return 0;
}

int labtwin_environment_rule_set_json(const char *input, char *out, size_t out_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    const char *id;
    cJSON *trigger_item;
    cJSON *clear_item;
    float trigger;
    float clear;
    int index;
    int ret;
    if (!root || !(id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rule_id"))) ||
        !cJSON_IsNumber(trigger_item = cJSON_GetObjectItem(root, "trigger")) ||
        !cJSON_IsNumber(clear_item = cJSON_GetObjectItem(root, "clear"))) goto invalid;
    trigger = (float)trigger_item->valuedouble;
    clear = (float)clear_item->valuedouble;
    pthread_mutex_lock(&g_env_lock);
    index = rule_index(id);
    if (index < 0 || !valid_rule_values(&g_rules[index], trigger, clear)) {
        pthread_mutex_unlock(&g_env_lock); goto invalid;
    }
    g_rules[index].trigger = trigger;
    g_rules[index].clear = clear;
    g_rules[index].pending_count = 0;
    ret = persist_config_locked();
    if (ret) g_storage_error = true;
    else g_config_fallback = false;
    response(out, out_size, ret == 0, ret ? "STORAGE_ERROR" : "OK",
             ret ? "rule not persisted" : "rule updated", NULL);
    pthread_mutex_unlock(&g_env_lock); cJSON_Delete(root); return ret;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid rule values", NULL); return -EINVAL;
}

int labtwin_environment_rules_set_json(const char *input, char *out,
                                       size_t out_size)
{
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *rules = root ? cJSON_GetObjectItem(root, "rules") : NULL;
    float triggers[LABTWIN_ENV_RULE_COUNT];
    float clears[LABTWIN_ENV_RULE_COUNT];
    float old_triggers[LABTWIN_ENV_RULE_COUNT];
    float old_clears[LABTWIN_ENV_RULE_COUNT];
    bool seen[LABTWIN_ENV_RULE_COUNT] = { false };
    int i;
    int ret;

    if (!cJSON_IsArray(rules) ||
        cJSON_GetArraySize(rules) != LABTWIN_ENV_RULE_COUNT)
        goto invalid;

    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        cJSON *rule = cJSON_GetArrayItem(rules, i);
        const char *id = cJSON_GetStringValue(
            cJSON_GetObjectItem(rule, "rule_id"));
        cJSON *trigger_item = cJSON_GetObjectItem(rule, "trigger");
        cJSON *clear_item = cJSON_GetObjectItem(rule, "clear");
        int index = rule_index(id);
        float trigger;
        float clear;

        if (index < 0 || seen[index] || !cJSON_IsNumber(trigger_item) ||
            !cJSON_IsNumber(clear_item)) {
            pthread_mutex_unlock(&g_env_lock);
            goto invalid;
        }
        trigger = (float)trigger_item->valuedouble;
        clear = (float)clear_item->valuedouble;
        if (!valid_rule_values(&g_rules[index], trigger, clear)) {
            pthread_mutex_unlock(&g_env_lock);
            goto invalid;
        }
        seen[index] = true;
        triggers[index] = trigger;
        clears[index] = clear;
    }

    for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
        old_triggers[i] = g_rules[i].trigger;
        old_clears[i] = g_rules[i].clear;
        g_rules[i].trigger = triggers[i];
        g_rules[i].clear = clears[i];
        g_rules[i].pending_count = 0;
        g_rules[i].clear_count = 0;
    }

    /* Persist the complete rule set once.  The former HTTP path performed
     * four fsync/rename cycles while holding this lock, which can stall the
     * board's NAND filesystem and freeze all services. */
    ret = persist_config_locked();
    if (ret) {
        for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
            g_rules[i].trigger = old_triggers[i];
            g_rules[i].clear = old_clears[i];
        }
        g_storage_error = true;
    } else {
        g_config_fallback = false;
    }
    response(out, out_size, ret == 0, ret ? "STORAGE_ERROR" : "OK",
             ret ? "rules not persisted" : "rules updated", NULL);
    pthread_mutex_unlock(&g_env_lock);
    cJSON_Delete(root);
    return ret;

invalid:
    cJSON_Delete(root);
    response(out, out_size, false, "INVALID_ARGUMENT",
             "invalid environment rules", NULL);
    return -EINVAL;
}

int labtwin_environment_rule_reset_json(char *out, size_t out_size)
{
    int ret;
    pthread_mutex_lock(&g_env_lock);
    defaults_locked();
    ret = persist_config_locked();
    if (ret) g_storage_error = true;
    else g_config_fallback = false;
    response(out, out_size, ret == 0, ret ? "STORAGE_ERROR" : "OK",
             ret ? "defaults not persisted" : "rules reset", NULL);
    pthread_mutex_unlock(&g_env_lock);
    return ret;
}

int labtwin_environment_get_active(labtwin_environment_event_view_t *event)
{
    int i;
    int best = -1;
    if (!event) return -EINVAL;
    pthread_mutex_lock(&g_env_lock);
    for (i = 0; i < ENV_MAX_EVENTS; i++) {
        if (g_events[i].used && g_events[i].view.state == LABTWIN_ENV_EVENT_ACTIVE) {
            if (best < 0 || strcmp(g_events[i].view.event_id,
                                   g_events[best].view.event_id) < 0)
                best = i;
        }
    }
    if (best >= 0) {
        *event = g_events[best].view;
        pthread_mutex_unlock(&g_env_lock); return 0;
    }
    pthread_mutex_unlock(&g_env_lock); return -ENOENT;
}

int labtwin_environment_get_recovery_notice(labtwin_environment_event_view_t *event)
{
    if (!event) return -EINVAL;
    pthread_mutex_lock(&g_env_lock);
    if (g_notice_index >= 0 && monotonic_ms() < g_notice_until_ms &&
        g_events[g_notice_index].used) {
        *event = g_events[g_notice_index].view;
        pthread_mutex_unlock(&g_env_lock); return 0;
    }
    pthread_mutex_unlock(&g_env_lock); return -ENOENT;
}

bool labtwin_environment_has_storage_error(void)
{
    bool value;
    pthread_mutex_lock(&g_env_lock);
    value = g_storage_error;
    pthread_mutex_unlock(&g_env_lock);
    return value;
}

#ifdef CONFIG_AI_AGENT_LABTWIN_ENV_TEST
int labtwin_environment_inject(const char *sensor, float value, bool clear)
{
    uint64_t now = monotonic_ms();
    env_sample_t *sample;
    pthread_mutex_lock(&g_env_lock);
    if (clear) {
        memset(g_injected, 0, sizeof(g_injected));
        memset(g_samples, 0, sizeof(g_samples));
        pthread_mutex_unlock(&g_env_lock); return 0;
    }
    if (sensor && strcmp(sensor, "temperature") == 0) sample = &g_samples[SENSOR_TEMPERATURE];
    else if (sensor && strcmp(sensor, "humidity") == 0) sample = &g_samples[SENSOR_HUMIDITY];
    else { pthread_mutex_unlock(&g_env_lock); return -EINVAL; }
    sample->value = value;
    sample->valid = isfinite(value) &&
        ((sample == &g_samples[SENSOR_TEMPERATURE] && value >= -40 && value <= 125) ||
         (sample == &g_samples[SENSOR_HUMIDITY] && value >= 0 && value <= 100));
    sample->sampled_ms = now;
    sample->seq++;
    if (!sample->valid) sample->failures++;
    else {
        int index = sample == &g_samples[SENSOR_TEMPERATURE] ?
            SENSOR_TEMPERATURE : SENSOR_HUMIDITY;
        g_injected[index] = true;
        g_injected_value[index] = value;
    }
    pthread_mutex_unlock(&g_env_lock);
    return sample->valid ? 0 : -EINVAL;
}
#endif

#ifdef LABTWIN_ENV_HOST_TEST
void labtwin_environment_process_test(unsigned long long now_ms)
{
    pthread_mutex_lock(&g_env_lock);
    process_locked((uint64_t)now_ms);
    telemetry_collect_locked((uint64_t)now_ms);
    pthread_mutex_unlock(&g_env_lock);
}

void labtwin_environment_telemetry_sample_test(bool temperature_valid,
                                               float temperature_c,
                                               bool humidity_valid,
                                               float humidity_percent,
                                               unsigned long long now_ms)
{
    pthread_mutex_lock(&g_env_lock);
    if (temperature_valid) {
        g_samples[SENSOR_TEMPERATURE].valid = true;
        g_samples[SENSOR_TEMPERATURE].value = temperature_c;
        g_samples[SENSOR_TEMPERATURE].sampled_ms = (uint64_t)now_ms;
        g_samples[SENSOR_TEMPERATURE].seq++;
    }
    if (humidity_valid) {
        g_samples[SENSOR_HUMIDITY].valid = true;
        g_samples[SENSOR_HUMIDITY].value = humidity_percent;
        g_samples[SENSOR_HUMIDITY].sampled_ms = (uint64_t)now_ms;
        g_samples[SENSOR_HUMIDITY].seq++;
    }
    telemetry_collect_locked((uint64_t)now_ms);
    pthread_mutex_unlock(&g_env_lock);
}

void labtwin_environment_shutdown_test(void)
{
    pthread_mutex_lock(&g_env_lock);
    g_initialized = false; g_running = false; g_storage_error = false;
    g_config_fallback = false;
    g_event_seq = 0; g_notice_index = -1; g_notice_until_ms = 0;
    memset(g_samples, 0, sizeof(g_samples));
    memset(g_telemetry, 0, sizeof(g_telemetry));
    g_telemetry_start = 0; g_telemetry_count = 0; g_telemetry_seq = 0;
    g_telemetry_active_count = 0; g_telemetry_hourly_count = 0;
    g_telemetry_window_until_ms = 0;
    g_telemetry_temperature_sum = 0.0f; g_telemetry_humidity_sum = 0.0f;
    g_telemetry_temperature_count = 0; g_telemetry_humidity_count = 0;
    g_telemetry_stale_count = 0;
    memset(g_injected, 0, sizeof(g_injected));
    memset(g_events, 0, sizeof(g_events));
    memset(g_foreground, 0, sizeof(g_foreground));
    defaults_locked();
    {
        int i;
        for (i = 0; i < LABTWIN_ENV_RULE_COUNT; i++) {
            g_rules[i].last_sample_seq = 0;
            g_rules[i].cooldown_until_ms = 0;
            g_rules[i].cooldown_until_epoch = 0;
        }
    }
    pthread_mutex_unlock(&g_env_lock);
}
#endif
