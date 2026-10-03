#include "labtwin/labtwin.h"
#include "labtwin/labtwin_environment.h"

#ifdef LABTWIN_HOST_TEST
#include <syslog.h>
#ifndef OK
#define OK 0
#endif
#else
#include "agent_compat.h"
#endif
#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

#define TAG "labtwin"
#ifndef LABTWIN_ROOT
#ifdef LABTWIN_HOST_TEST
#define LABTWIN_ROOT "/tmp/labtwin-m2-test-20260714-v2"
#else
#define LABTWIN_ROOT "/data/labtwin"
#endif
#endif
#define LABTWIN_EXPERIMENTS LABTWIN_ROOT "/experiments"
#define LABTWIN_SCHEMA_VERSION 2
#define LABTWIN_TRUST_EPOCH 1735689600LL
#define LABTWIN_MAX_DURATION (7 * 24 * 60 * 60)
#define LABTWIN_EVENT_BUFFER 16384
/* Timer expiry persists a complete experiment event and snapshot.  It must
 * not run on NuttX's 2 KiB default pthread stack: cJSON serialization and
 * the persistence helpers have materially larger stack frames. */
#define LABTWIN_TIMER_THREAD_STACKSIZE (16 * 1024)

typedef struct {
    char title[96];
    bool completed;
} labtwin_step_t;

typedef struct {
    char id[16];
    char label[64];
    uint8_t step_index;
    labtwin_timer_state_t state;
    uint32_t remaining_seconds;
    int64_t due_epoch;
    uint64_t deadline_ms;
} labtwin_timer_t;

typedef struct {
    bool used;
    bool recovery_error;
    bool snapshot_pending;
    char id[32];
    char name[64];
    char description[513];
    char recovery_reason[32];
    char last_observation[513];
    char last_observation_structured[1025];
    labtwin_state_t state;
    uint8_t current_step;
    uint8_t step_count;
    labtwin_step_t steps[LABTWIN_MAX_STEPS];
    uint8_t timer_count;
    labtwin_timer_t timers[LABTWIN_MAX_TIMERS];
    uint64_t last_event_seq;
    int64_t created_epoch;
    int64_t planned_start_epoch;
    int64_t planned_end_epoch;
    int64_t started_epoch;
    int64_t ended_epoch;
    int64_t updated_epoch;
} labtwin_experiment_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static labtwin_experiment_t g_experiments[LABTWIN_MAX_EXPERIMENTS];
/* A single staging object under g_lock avoids large per-thread stack copies.
 * No mutation is visible until its complete event has been synced. */
static labtwin_experiment_t g_candidate;

static labtwin_experiment_t *stage(labtwin_experiment_t *live)
{
    g_candidate = *live;
    return &g_candidate;
}
static labtwin_sensor_snapshot_t g_sensors;
static pthread_t g_thread;
static bool g_initialized;
static bool g_thread_running;
static int g_focused = -1;
static uint32_t g_id_counter = 1;
static int64_t g_last_wall_epoch;
static uint64_t g_recovery_deadline_ms;
static uint64_t g_last_checkpoint_ms;

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

bool labtwin_clock_trusted(void)
{
    int64_t now = wall_epoch();
    return now >= LABTWIN_TRUST_EPOCH &&
           (g_last_wall_epoch == 0 || now + 2 >= g_last_wall_epoch);
}

const char *labtwin_state_name(labtwin_state_t state)
{
    switch (state) {
    case LABTWIN_STATE_READY: return "READY";
    case LABTWIN_STATE_RUNNING: return "RUNNING";
    case LABTWIN_STATE_PAUSED: return "PAUSED";
    case LABTWIN_STATE_COMPLETED: return "COMPLETED";
    case LABTWIN_STATE_CANCELLED: return "CANCELLED";
    default: return "RECOVERY_ERROR";
    }
}

static const char *timer_state_name(labtwin_timer_state_t state)
{
    switch (state) {
    case LABTWIN_TIMER_RUNNING: return "RUNNING";
    case LABTWIN_TIMER_PAUSED: return "PAUSED";
    case LABTWIN_TIMER_EXPIRED: return "EXPIRED";
    default: return "CANCELLED";
    }
}

static int mkdir_checked(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -errno;
    chmod(path, 0700);
    return 0;
}

static bool valid_id(const char *id)
{
    size_t i, len;
    if (!id || (len = strlen(id)) == 0 || len >= 32) return false;
    for (i = 0; i < len; i++) {
        if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_')
            return false;
    }
    return true;
}

static bool string_ok(const cJSON *item, size_t max_len)
{
    return cJSON_IsString(item) && item->valuestring &&
           item->valuestring[0] && strlen(item->valuestring) < max_len;
}

static bool fields_allowed(cJSON *root, const char *const *allowed,
                           size_t count)
{
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        size_t i;
        bool found = false;
        for (i = 0; i < count; i++) {
            if (strcmp(item->string, allowed[i]) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
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

static labtwin_experiment_t *find_experiment(const char *id)
{
    int i;
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++)
        if (g_experiments[i].used && strcmp(g_experiments[i].id, id) == 0)
            return &g_experiments[i];
    return NULL;
}

static void add_epoch_or_null(cJSON *root, const char *name, int64_t epoch)
{
    if (epoch > 0)
        cJSON_AddNumberToObject(root, name, (double)epoch);
    else
        cJSON_AddNullToObject(root, name);
}

static cJSON *experiment_json(const labtwin_experiment_t *exp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *steps = cJSON_AddArrayToObject(root, "steps");
    cJSON *timers = cJSON_AddArrayToObject(root, "timers");
    int i;
    cJSON_AddNumberToObject(root, "schema_version", LABTWIN_SCHEMA_VERSION);
    cJSON_AddStringToObject(root, "experiment_id", exp->id);
    cJSON_AddStringToObject(root, "name", exp->name);
    cJSON_AddStringToObject(root, "description", exp->description);
    cJSON_AddStringToObject(root, "state", labtwin_state_name(exp->state));
    cJSON_AddNumberToObject(root, "current_step", exp->current_step);
    cJSON_AddStringToObject(root, "recovery_reason", exp->recovery_reason);
    cJSON_AddStringToObject(root, "last_observation", exp->last_observation);
    if (exp->last_observation_structured[0]) {
        cJSON *structured = cJSON_Parse(exp->last_observation_structured);
        if (structured)
            cJSON_AddItemToObject(root, "last_observation_structured", structured);
    }
    cJSON_AddNumberToObject(root, "last_event_seq", (double)exp->last_event_seq);
    cJSON_AddBoolToObject(root, "snapshot_pending", exp->snapshot_pending);
    add_epoch_or_null(root, "created_epoch", exp->created_epoch);
    add_epoch_or_null(root, "planned_start_epoch", exp->planned_start_epoch);
    add_epoch_or_null(root, "planned_end_epoch", exp->planned_end_epoch);
    add_epoch_or_null(root, "started_epoch", exp->started_epoch);
    add_epoch_or_null(root, "ended_epoch", exp->ended_epoch);
    add_epoch_or_null(root, "updated_epoch", exp->updated_epoch);
    for (i = 0; i < exp->step_count; i++) {
        cJSON *step = cJSON_CreateObject();
        cJSON_AddStringToObject(step, "title", exp->steps[i].title);
        cJSON_AddBoolToObject(step, "completed", exp->steps[i].completed);
        cJSON_AddItemToArray(steps, step);
    }
    for (i = 0; i < exp->timer_count; i++) {
        const labtwin_timer_t *t = &exp->timers[i];
        cJSON *timer = cJSON_CreateObject();
        cJSON_AddStringToObject(timer, "timer_id", t->id);
        cJSON_AddStringToObject(timer, "label", t->label);
        cJSON_AddNumberToObject(timer, "step_index", t->step_index);
        cJSON_AddStringToObject(timer, "state", timer_state_name(t->state));
        cJSON_AddNumberToObject(timer, "remaining_seconds", t->remaining_seconds);
        cJSON_AddNumberToObject(timer, "due_epoch", (double)t->due_epoch);
        cJSON_AddItemToArray(timers, timer);
    }
    return root;
}

static labtwin_state_t parse_state(const char *value)
{
    if (!value) return LABTWIN_STATE_RECOVERY_ERROR;
    if (!strcmp(value, "READY")) return LABTWIN_STATE_READY;
    if (!strcmp(value, "RUNNING")) return LABTWIN_STATE_RUNNING;
    if (!strcmp(value, "PAUSED")) return LABTWIN_STATE_PAUSED;
    if (!strcmp(value, "COMPLETED")) return LABTWIN_STATE_COMPLETED;
    if (!strcmp(value, "CANCELLED")) return LABTWIN_STATE_CANCELLED;
    return LABTWIN_STATE_RECOVERY_ERROR;
}

static labtwin_timer_state_t parse_timer_state(const char *value)
{
    if (!value || !strcmp(value, "CANCELLED")) return LABTWIN_TIMER_CANCELLED;
    if (!strcmp(value, "RUNNING")) return LABTWIN_TIMER_RUNNING;
    if (!strcmp(value, "PAUSED")) return LABTWIN_TIMER_PAUSED;
    if (!strcmp(value, "EXPIRED")) return LABTWIN_TIMER_EXPIRED;
    return LABTWIN_TIMER_CANCELLED;
}

static int64_t json_epoch(const cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    return cJSON_IsNumber(item) && item->valuedouble > 0
        ? (int64_t)item->valuedouble : 0;
}

static int experiment_from_json(labtwin_experiment_t *exp, cJSON *root)
{
    cJSON *id = cJSON_GetObjectItem(root, "experiment_id");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *state = cJSON_GetObjectItem(root, "state");
    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    cJSON *timers = cJSON_GetObjectItem(root, "timers");
    cJSON *item;
    int i = 0;
    if (!string_ok(id, sizeof(exp->id)) || !string_ok(name, sizeof(exp->name)) ||
        !cJSON_IsString(state) || !cJSON_IsArray(steps) ||
        cJSON_GetArraySize(steps) < 1 ||
        cJSON_GetArraySize(steps) > LABTWIN_MAX_STEPS) return -EINVAL;
    memset(exp, 0, sizeof(*exp));
    exp->used = true;
    snprintf(exp->id, sizeof(exp->id), "%s", id->valuestring);
    snprintf(exp->name, sizeof(exp->name), "%s", name->valuestring);
    item = cJSON_GetObjectItem(root, "description");
    if (cJSON_IsString(item)) snprintf(exp->description,
        sizeof(exp->description), "%s", item->valuestring);
    exp->state = parse_state(state->valuestring);
    exp->current_step = (uint8_t)cJSON_GetNumberValue(
        cJSON_GetObjectItem(root, "current_step"));
    exp->last_event_seq = (uint64_t)cJSON_GetNumberValue(
        cJSON_GetObjectItem(root, "last_event_seq"));
    exp->updated_epoch = (int64_t)cJSON_GetNumberValue(
        cJSON_GetObjectItem(root, "updated_epoch"));
    exp->created_epoch = json_epoch(root, "created_epoch");
    exp->planned_start_epoch = json_epoch(root, "planned_start_epoch");
    exp->planned_end_epoch = json_epoch(root, "planned_end_epoch");
    exp->started_epoch = json_epoch(root, "started_epoch");
    exp->ended_epoch = json_epoch(root, "ended_epoch");
    item = cJSON_GetObjectItem(root, "recovery_reason");
    if (cJSON_IsString(item)) snprintf(exp->recovery_reason,
        sizeof(exp->recovery_reason), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "last_observation");
    if (cJSON_IsString(item)) snprintf(exp->last_observation,
        sizeof(exp->last_observation), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "last_observation_structured");
    if (cJSON_IsObject(item)) {
        char *structured = cJSON_PrintUnformatted(item);
        if (structured) {
            snprintf(exp->last_observation_structured,
                sizeof(exp->last_observation_structured), "%s", structured);
            free(structured);
        }
    }
    cJSON_ArrayForEach(item, steps) {
        cJSON *title = cJSON_GetObjectItem(item, "title");
        if (!string_ok(title, sizeof(exp->steps[i].title))) return -EINVAL;
        snprintf(exp->steps[i].title, sizeof(exp->steps[i].title), "%s",
                 title->valuestring);
        exp->steps[i].completed = cJSON_IsTrue(
            cJSON_GetObjectItem(item, "completed"));
        i++;
    }
    exp->step_count = (uint8_t)i;
    if (exp->current_step >= exp->step_count) exp->current_step = exp->step_count - 1;
    i = 0;
    if (cJSON_IsArray(timers)) cJSON_ArrayForEach(item, timers) {
        labtwin_timer_t *t;
        cJSON *tid;
        cJSON *label;
        if (i >= LABTWIN_MAX_TIMERS) break;
        t = &exp->timers[i];
        tid = cJSON_GetObjectItem(item, "timer_id");
        label = cJSON_GetObjectItem(item, "label");
        if (!string_ok(tid, sizeof(t->id)) || !string_ok(label, sizeof(t->label)))
            return -EINVAL;
        snprintf(t->id, sizeof(t->id), "%s", tid->valuestring);
        snprintf(t->label, sizeof(t->label), "%s", label->valuestring);
        t->step_index = (uint8_t)cJSON_GetNumberValue(
            cJSON_GetObjectItem(item, "step_index"));
        t->state = parse_timer_state(cJSON_GetStringValue(
            cJSON_GetObjectItem(item, "state")));
        t->remaining_seconds = (uint32_t)cJSON_GetNumberValue(
            cJSON_GetObjectItem(item, "remaining_seconds"));
        t->due_epoch = (int64_t)cJSON_GetNumberValue(
            cJSON_GetObjectItem(item, "due_epoch"));
        i++;
    }
    exp->timer_count = (uint8_t)i;
    return 0;
}

static int write_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -EIO;
        off += (size_t)n;
    }
    return 0;
}

static int persist_snapshot(labtwin_experiment_t *exp)
{
    char dir[128], path[160], tmp[164];
    cJSON *root = experiment_json(exp);
    char *text = cJSON_PrintUnformatted(root);
    int fd, ret = 0;
    snprintf(dir, sizeof(dir), "%s/%s", LABTWIN_EXPERIMENTS, exp->id);
    mkdir_checked(dir);
    snprintf(path, sizeof(path), "%s/snapshot.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !text) ret = -EIO;
    else {
        ret = write_all(fd, text, strlen(text));
        if (!ret && fsync(fd) < 0) ret = -EIO;
        if (close(fd) < 0) ret = -EIO;
        fd = -1;
        if (!ret && rename(tmp, path) < 0) ret = -errno;
        chmod(path, 0600);
    }
    if (fd >= 0) close(fd);
    free(text);
    cJSON_Delete(root);
    return ret;
}

static int append_event(labtwin_experiment_t *exp, const char *type,
                        const char *source)
{
    char dir[128], path[160], event_id[24];
    cJSON *root = cJSON_CreateObject();
    char *text;
    int fd, ret;
    off_t original_size = -1;
    exp->last_event_seq++;
    exp->updated_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    snprintf(event_id, sizeof(event_id), "%08lx-%08lx",
             (unsigned long)exp->last_event_seq,
             (unsigned long)(monotonic_ms() & 0xffffffffUL));
    cJSON_AddNumberToObject(root, "schema_version", LABTWIN_SCHEMA_VERSION);
    cJSON_AddNumberToObject(root, "seq", (double)exp->last_event_seq);
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "experiment_id", exp->id);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "source", source ? source : "system");
    if (labtwin_clock_trusted()) cJSON_AddNumberToObject(root, "timestamp_epoch", (double)wall_epoch());
    else cJSON_AddNullToObject(root, "timestamp_epoch");
    cJSON_AddNumberToObject(root, "uptime_ms", (double)monotonic_ms());
    cJSON_AddItemToObject(root, "payload", experiment_json(exp));
    text = cJSON_PrintUnformatted(root);
    snprintf(dir, sizeof(dir), "%s/%s", LABTWIN_EXPERIMENTS, exp->id);
    mkdir_checked(dir);
    snprintf(path, sizeof(path), "%s/events.jsonl", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) original_size = lseek(fd, 0, SEEK_END);
    ret = (fd < 0 || !text) ? -EIO : write_all(fd, text, strlen(text));
    if (!ret) ret = write_all(fd, "\n", 1);
    if (fd >= 0) {
        if (!ret && fsync(fd) < 0) ret = -EIO;
        if (ret && (original_size < 0 || ftruncate(fd, original_size) < 0 ||
                    fsync(fd) < 0)) {
            labtwin_experiment_t *live = find_experiment(exp->id);
            if (live) {
                live->recovery_error = true;
                live->state = LABTWIN_STATE_RECOVERY_ERROR;
                snprintf(live->recovery_reason, sizeof(live->recovery_reason),
                         "journal_rollback_uncertain");
            }
            ret = -EUCLEAN;
        }
        close(fd);
        chmod(path, 0600);
    }
    free(text);
    cJSON_Delete(root);
    if (ret) exp->last_event_seq--;
    return ret;
}

static int commit(labtwin_experiment_t *exp, const char *type,
                  const char *source)
{
    labtwin_experiment_t *live = find_experiment(exp->id);
    int ret;
    if (live && live->recovery_error) return -EUCLEAN;
    exp->snapshot_pending = false;
    ret = append_event(exp, type, source);
    if (ret) return ret;
    /* The journal is committed. A snapshot failure must never invite a
     * retry of this mutation; recovery can replay even without a snapshot. */
    exp->snapshot_pending = persist_snapshot(exp) != 0;
    if (live) *live = *exp;
    return 0;
}

static int load_one(const char *id, labtwin_experiment_t *exp)
{
    char path[160];
    FILE *file;
    long size;
    char *buffer;
    cJSON *root;
    int ret;
    snprintf(path, sizeof(path), "%s/%s/snapshot.json", LABTWIN_EXPERIMENTS, id);
    file = fopen(path, "r");
    memset(exp, 0, sizeof(*exp));
    ret = -ENOENT;
    if (file) {
        fseek(file, 0, SEEK_END); size = ftell(file); fseek(file, 0, SEEK_SET);
        if (size > 0 && size < LABTWIN_EVENT_BUFFER) {
            buffer = malloc((size_t)size + 1);
            if (!buffer) { fclose(file); return -ENOMEM; }
            buffer[fread(buffer, 1, (size_t)size, file)] = 0;
            root = cJSON_Parse(buffer); free(buffer);
            ret = root ? experiment_from_json(exp, root) : -EINVAL;
            if (!ret && strcmp(exp->id, id)) ret = -EINVAL;
            cJSON_Delete(root);
        }
        fclose(file);
    }
    if (ret) memset(exp, 0, sizeof(*exp));

    /* Every event carries a full post-mutation state.  Replaying the last
     * valid event after the snapshot therefore repairs a power loss between
     * event append and snapshot rename. */
    snprintf(path, sizeof(path), "%s/%s/events.jsonl", LABTWIN_EXPERIMENTS, id);
    file = fopen(path, "r");
    if (file) {
        char *line = malloc(LABTWIN_EVENT_BUFFER);
        labtwin_experiment_t *replayed = malloc(sizeof(*replayed));
        bool bad_middle = false;
        bool truncated = false;
        long valid_offset = 0;
        if (!line || !replayed) {
            free(line); free(replayed); fclose(file); return -ENOMEM;
        }
        while (fgets(line, LABTWIN_EVENT_BUFFER, file)) {
            size_t len = strlen(line);
            cJSON *event;
            cJSON *seq;
            cJSON *payload;
            if (!len || line[len - 1] != '\n') { truncated = true; break; }
            event = cJSON_Parse(line);
            if (!event) { bad_middle = true; break; }
            seq = cJSON_GetObjectItem(event, "seq");
            payload = cJSON_GetObjectItem(event, "payload");
            if (cJSON_IsNumber(seq) && payload &&
                (uint64_t)seq->valuedouble > exp->last_event_seq) {
                if (experiment_from_json(replayed, payload) == 0 &&
                    !strcmp(replayed->id, id) &&
                    replayed->last_event_seq == (uint64_t)seq->valuedouble) {
                    *exp = *replayed;
                    exp->snapshot_pending = true;
                    ret = 0;
                }
                else bad_middle = true;
            }
            cJSON_Delete(event);
            if (bad_middle) break;
            valid_offset = ftell(file);
        }
        fclose(file);
        /* Never append a new event behind an incomplete old JSON line. */
        if (truncated && !bad_middle) {
            int fd = open(path, O_WRONLY);
            if (fd < 0) bad_middle = true;
            else {
                if (ftruncate(fd, valid_offset) || fsync(fd)) bad_middle = true;
                close(fd);
            }
        }
        free(line); free(replayed);
        if (bad_middle) {
            exp->recovery_error = true;
            exp->state = LABTWIN_STATE_RECOVERY_ERROR;
            snprintf(exp->recovery_reason, sizeof(exp->recovery_reason),
                     "event_log_corrupt");
        }
    }
    return exp->used ? 0 : ret;
}

static void recover_timers(labtwin_experiment_t *exp)
{
    int i;
    uint64_t now_ms = monotonic_ms();
    int64_t now = wall_epoch();
    bool trusted = labtwin_clock_trusted();
    for (i = 0; i < exp->timer_count; i++) {
        labtwin_timer_t *t = &exp->timers[i];
        if (t->state != LABTWIN_TIMER_RUNNING) continue;
        if (trusted && t->due_epoch > 0) {
            if (now >= t->due_epoch) {
                t->remaining_seconds = 0;
                t->state = LABTWIN_TIMER_EXPIRED;
            } else {
                t->remaining_seconds = (uint32_t)(t->due_epoch - now);
                t->deadline_ms = now_ms + (uint64_t)t->remaining_seconds * 1000ULL;
            }
        } else {
            t->deadline_ms = UINT64_MAX;
            snprintf(exp->recovery_reason, sizeof(exp->recovery_reason),
                     "waiting_for_time");
        }
    }
}

static bool runtime_active(const labtwin_experiment_t *exp)
{
    return exp->state == LABTWIN_STATE_RUNNING ||
           exp->state == LABTWIN_STATE_PAUSED;
}

/* The RAM cache is deliberately smaller than retained history.  Terminal
 * experiments remain authoritative on disk and are loaded on demand. */
static int cache_slot_for_locked(const labtwin_experiment_t *candidate)
{
    int i;
    int oldest = -1;
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        if (!g_experiments[i].used) return i;
        if (!runtime_active(&g_experiments[i]) &&
            (oldest < 0 || g_experiments[i].updated_epoch <
                           g_experiments[oldest].updated_epoch))
            oldest = i;
    }
    (void)candidate;
    return oldest;
}

static labtwin_experiment_t *find_or_load_experiment_locked(const char *id)
{
    labtwin_experiment_t loaded;
    labtwin_experiment_t *existing = find_experiment(id);
    int slot;
    if (existing) return existing;
    if (load_one(id, &loaded) != 0) return NULL;
    slot = cache_slot_for_locked(&loaded);
    if (slot < 0) return NULL;
    g_experiments[slot] = loaded;
    return &g_experiments[slot];
}

static void load_all(void)
{
    DIR *dir = opendir(LABTWIN_EXPERIMENTS);
    struct dirent *entry;
    int slot = 0;
    int i;
    if (!dir) return;
    while ((entry = readdir(dir)) && slot < LABTWIN_MAX_EXPERIMENTS) {
        if (entry->d_name[0] == '.') continue;
        labtwin_experiment_t candidate;
        if (load_one(entry->d_name, &candidate) == 0 && runtime_active(&candidate))
            g_experiments[slot++] = candidate;
    }
    rewinddir(dir);
    while ((entry = readdir(dir)) && slot < LABTWIN_MAX_EXPERIMENTS) {
        if (entry->d_name[0] == '.') continue;
        labtwin_experiment_t candidate;
        if (load_one(entry->d_name, &candidate) == 0 &&
            candidate.state == LABTWIN_STATE_READY)
            g_experiments[slot++] = candidate;
    }
    closedir(dir);

    /* A reboot can expose a factory/default clock that is still after the
     * coarse 2025 threshold.  Seed the monotonic wall-clock guard from every
     * persisted experiment before recovering any timer, so that a clock that
     * moved backwards cannot be treated as trusted until NTP catches up. */
    for (i = 0; i < slot; i++) {
        if (g_experiments[i].updated_epoch > g_last_wall_epoch)
            g_last_wall_epoch = g_experiments[i].updated_epoch;
    }
    for (i = 0; i < slot; i++) {
        recover_timers(&g_experiments[i]);
        if (g_focused < 0 && g_experiments[i].state != LABTWIN_STATE_COMPLETED &&
            g_experiments[i].state != LABTWIN_STATE_CANCELLED)
            g_focused = i;
    }
}

static void *timer_thread(void *arg)
{
    (void)arg;
    while (g_thread_running) {
        int i, j;
        uint64_t now_ms = monotonic_ms();
        int64_t now = wall_epoch();
        pthread_mutex_lock(&g_lock);
        if (labtwin_clock_trusted()) g_last_wall_epoch = now;
        for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
            labtwin_experiment_t *exp = &g_experiments[i];
            if (!exp->used || exp->recovery_error) continue;
            for (j = 0; j < exp->timer_count; j++) {
                exp = stage(&g_experiments[i]);
                labtwin_timer_t *t = &exp->timers[j];
                if (t->state != LABTWIN_TIMER_RUNNING) continue;
                if (t->deadline_ms == UINT64_MAX) {
                    if (labtwin_clock_trusted() && t->due_epoch > 0) {
                        if (now >= t->due_epoch) {
                            t->remaining_seconds = 0;
                            t->state = LABTWIN_TIMER_EXPIRED;
                        } else {
                            t->remaining_seconds = (uint32_t)(t->due_epoch - now);
                            t->deadline_ms = now_ms + (uint64_t)t->remaining_seconds * 1000ULL;
                        }
                        exp->recovery_reason[0] = 0;
                        commit(exp, "TIMER_RECOVERED", "system");
                    } else if (now_ms >= g_recovery_deadline_ms) {
                        t->state = LABTWIN_TIMER_PAUSED;
                        if (exp->state == LABTWIN_STATE_RUNNING)
                            exp->state = LABTWIN_STATE_PAUSED;
                        snprintf(exp->recovery_reason,
                                 sizeof(exp->recovery_reason), "time_untrusted");
                        commit(exp, "TIMER_RECOVERY_PAUSED", "system");
                    }
                    continue;
                }
                if (now_ms >= t->deadline_ms) {
                    t->remaining_seconds = 0;
                    t->state = LABTWIN_TIMER_EXPIRED;
                    commit(exp, "TIMER_EXPIRED", "system");
                } else {
                    t->remaining_seconds = (uint32_t)((t->deadline_ms - now_ms + 999) / 1000);
                    g_experiments[i].timers[j].remaining_seconds = t->remaining_seconds;
                }
            }
            exp = &g_experiments[i];
            if (now_ms - g_last_checkpoint_ms >= 60000) {
                bool untrusted_active = false;
                for (j = 0; j < exp->timer_count; j++)
                    if (exp->timers[j].state == LABTWIN_TIMER_RUNNING &&
                        exp->timers[j].due_epoch == 0) untrusted_active = true;
                if (untrusted_active || exp->snapshot_pending) {
                    if (persist_snapshot(exp) == 0) exp->snapshot_pending = false;
                }
            }
        }
        if (now_ms - g_last_checkpoint_ms >= 60000) g_last_checkpoint_ms = now_ms;
        pthread_mutex_unlock(&g_lock);
        sleep(1);
    }
    return NULL;
}

int labtwin_service_init(void)
{
    int env_ret;
    int thread_ret;
    pthread_attr_t timer_thread_attr;
    pthread_mutex_lock(&g_lock);
    if (g_initialized) {
        pthread_mutex_unlock(&g_lock);
        return labtwin_environment_init();
    }
    mkdir_checked("/data");
    mkdir_checked(LABTWIN_ROOT);
    mkdir_checked(LABTWIN_EXPERIMENTS);
    g_recovery_deadline_ms = monotonic_ms() + 120000ULL;
    g_last_checkpoint_ms = monotonic_ms();
    load_all();
    g_thread_running = true;
    pthread_attr_init(&timer_thread_attr);
    pthread_attr_setstacksize(&timer_thread_attr,
                              LABTWIN_TIMER_THREAD_STACKSIZE);
    thread_ret = pthread_create(&g_thread, &timer_thread_attr,
                                timer_thread, NULL);
    pthread_attr_destroy(&timer_thread_attr);
    if (thread_ret != 0) {
        g_thread_running = false;
        pthread_mutex_unlock(&g_lock);
        return -EIO;
    }
    pthread_detach(g_thread);
    g_initialized = true;
    pthread_mutex_unlock(&g_lock);
    env_ret = labtwin_environment_init();
    if (env_ret != 0) {
        syslog(LOG_ERR, "[%s] environment service failed: %d\n", TAG, env_ret);
        return env_ret;
    }
    syslog(LOG_INFO, "[%s] service ready\n", TAG);
    return 0;
}

static cJSON *view_json(const labtwin_experiment_t *exp)
{
    cJSON *data = experiment_json(exp);
    return data;
}

static bool experiment_storage_blocked(void)
{
#ifdef LABTWIN_HOST_TEST
    return false;
#else
    struct statfs storage;
    unsigned long long total;
    unsigned long long available;
    if (statfs("/data", &storage) != 0)
        return false;
    total = (unsigned long long)storage.f_blocks * storage.f_bsize;
    available = (unsigned long long)storage.f_bavail * storage.f_bsize;
    return total > 0 && (double)(total - available) / (double)total >= 0.95;
#endif
}

int labtwin_experiment_create_json(const char *input, char *out,
                                   size_t out_size, const char *source)
{
    static const char *const allowed[] = {
        "experiment_id", "name", "description", "planned_start_epoch",
        "planned_end_epoch", "steps"
    };
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id, *name, *steps, *item, *description, *planned_start, *planned_end;
    labtwin_experiment_t *exp = NULL;
    char generated[32];
    int i, slot = -1, active_count = 0;
    if (experiment_storage_blocked()) {
        response(out, out_size, false, "STORAGE_FULL",
                 "storage is above 95 percent; export or delete old experiments",
                 NULL);
        return -ENOSPC;
    }
    if (!root || !cJSON_IsObject(root) || !fields_allowed(root, allowed, 6))
        goto invalid;
    id = cJSON_GetObjectItem(root, "experiment_id");
    name = cJSON_GetObjectItem(root, "name");
    steps = cJSON_GetObjectItem(root, "steps");
    description = cJSON_GetObjectItem(root, "description");
    planned_start = cJSON_GetObjectItem(root, "planned_start_epoch");
    planned_end = cJSON_GetObjectItem(root, "planned_end_epoch");
    if (!string_ok(name, 64) || !cJSON_IsArray(steps) ||
        cJSON_GetArraySize(steps) < 1 || cJSON_GetArraySize(steps) > LABTWIN_MAX_STEPS)
        goto invalid;
    if (description && (!cJSON_IsString(description) ||
        strlen(description->valuestring) >= sizeof(exp->description))) goto invalid;
    /* The Portal serializes omitted datetime-local controls as JSON null.
     * Accept that canonical API representation as an unspecified schedule. */
    if (planned_start && !cJSON_IsNumber(planned_start) &&
        !cJSON_IsNull(planned_start)) goto invalid;
    if (planned_end && !cJSON_IsNumber(planned_end) &&
        !cJSON_IsNull(planned_end)) goto invalid;
    if (cJSON_IsNumber(planned_start) && cJSON_IsNumber(planned_end) &&
        planned_start->valuedouble > 0 &&
        planned_end->valuedouble > 0 &&
        planned_end->valuedouble < planned_start->valuedouble) goto invalid;
    if (id && (!cJSON_IsString(id) || !valid_id(id->valuestring))) goto invalid;
    pthread_mutex_lock(&g_lock);
    if (!id) {
        do { snprintf(generated, sizeof(generated), "EXP-%08lx",
                      (unsigned long)(monotonic_ms() + g_id_counter++)); }
        while (find_experiment(generated));
    }
    {
        labtwin_experiment_t persisted;
        const char *candidate_id = id ? id->valuestring : generated;
        if (find_experiment(candidate_id) || load_one(candidate_id, &persisted) == 0) {
        pthread_mutex_unlock(&g_lock);
        response(out, out_size, false, "ALREADY_EXISTS", "experiment already exists", NULL);
        cJSON_Delete(root); return -EEXIST;
        }
    }
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        if (g_experiments[i].used && runtime_active(&g_experiments[i])) active_count++;
        if (!g_experiments[i].used && slot < 0) slot = i;
    }
    if (active_count >= LABTWIN_MAX_ACTIVE_EXPERIMENTS) {
        pthread_mutex_unlock(&g_lock);
        response(out, out_size, false, "LIMIT_REACHED", "active experiment limit reached", NULL);
        cJSON_Delete(root); return -ENOSPC;
    }
    if (slot < 0) slot = cache_slot_for_locked(NULL);
    if (slot < 0) {
        pthread_mutex_unlock(&g_lock);
        response(out, out_size, false, "CACHE_BUSY", "all cached experiments are active", NULL);
        cJSON_Delete(root); return -EBUSY;
    }
    exp = &g_candidate; memset(exp, 0, sizeof(*exp)); exp->used = true;
    snprintf(exp->id, sizeof(exp->id), "%s", id ? id->valuestring : generated);
    snprintf(exp->name, sizeof(exp->name), "%s", name->valuestring);
    if (description) snprintf(exp->description, sizeof(exp->description), "%s",
                              description->valuestring);
    exp->planned_start_epoch = cJSON_IsNumber(planned_start) &&
        planned_start->valuedouble > 0 ?
        (int64_t)planned_start->valuedouble : 0;
    exp->planned_end_epoch = cJSON_IsNumber(planned_end) &&
        planned_end->valuedouble > 0 ?
        (int64_t)planned_end->valuedouble : 0;
    exp->created_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    exp->state = LABTWIN_STATE_READY;
    i = 0;
    cJSON_ArrayForEach(item, steps) {
        const char *title = cJSON_IsString(item) ? item->valuestring :
            cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
        if (!title || !title[0] || strlen(title) >= 96) {
            memset(exp, 0, sizeof(*exp)); pthread_mutex_unlock(&g_lock); goto invalid;
        }
        snprintf(exp->steps[i++].title, sizeof(exp->steps[0].title), "%s", title);
    }
    exp->step_count = (uint8_t)i;
    if (commit(exp, "EXPERIMENT_CREATED", source) != 0) {
        memset(exp, 0, sizeof(*exp)); pthread_mutex_unlock(&g_lock);
        response(out, out_size, false, "STORAGE_ERROR", "could not persist experiment", NULL);
        cJSON_Delete(root); return -EIO;
    }
    g_experiments[slot] = *exp;
    g_focused = slot;
    response(out, out_size, true, "OK", "experiment created", view_json(exp));
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return 0;
invalid:
    cJSON_Delete(root);
    response(out, out_size, false, "INVALID_ARGUMENT", "invalid experiment input", NULL);
    return -EINVAL;
}

int labtwin_experiment_get_json(const char *input, char *out, size_t out_size)
{
    static const char *const allowed[] = {"experiment_id"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id;
    labtwin_experiment_t *exp;
    if (!root || !fields_allowed(root, allowed, 1) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32)) {
        cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "experiment_id required", NULL); return -EINVAL;
    }
    pthread_mutex_lock(&g_lock); exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL);
    else response(out, out_size, true, "OK", "experiment found", view_json(exp));
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return exp ? 0 : -ENOENT;
}

int labtwin_experiment_list_json(const char *input, char *out, size_t out_size)
{
    static const char *const allowed[] = {"state"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *filter;
    cJSON *data, *array;
    int i;
    if (!root || !fields_allowed(root, allowed, 1)) {
        cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid filter", NULL); return -EINVAL;
    }
    filter = cJSON_GetObjectItem(root, "state");
    if (filter && !cJSON_IsString(filter)) {
        cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "state must be string", NULL); return -EINVAL;
    }
    data = cJSON_CreateObject(); array = cJSON_AddArrayToObject(data, "experiments");
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        labtwin_experiment_t *e = &g_experiments[i];
        cJSON *summary;
        if (!e->used || (filter && strcmp(filter->valuestring, labtwin_state_name(e->state)))) continue;
        summary = cJSON_CreateObject();
        cJSON_AddStringToObject(summary, "experiment_id", e->id);
        cJSON_AddStringToObject(summary, "name", e->name);
        cJSON_AddStringToObject(summary, "state", labtwin_state_name(e->state));
        cJSON_AddNumberToObject(summary, "current_step", e->current_step);
        cJSON_AddNumberToObject(summary, "step_count", e->step_count);
        cJSON_AddItemToArray(array, summary);
    }
    response(out, out_size, true, "OK", "experiments listed", data);
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return 0;
}

static bool text_contains_ci(const char *text, const char *needle)
{
    size_t needle_len;
    if (!needle || !needle[0]) return true;
    if (!text) return false;
    needle_len = strlen(needle);
    while (*text) {
        size_t i;
        for (i = 0; i < needle_len && text[i] &&
             tolower((unsigned char)text[i]) == tolower((unsigned char)needle[i]); i++) {}
        if (i == needle_len) return true;
        text++;
    }
    return false;
}

static int64_t calendar_epoch(const labtwin_experiment_t *exp)
{
    if (exp->planned_start_epoch) return exp->planned_start_epoch;
    if (exp->ended_epoch) return exp->ended_epoch;
    if (exp->started_epoch) return exp->started_epoch;
    if (exp->updated_epoch) return exp->updated_epoch;
    return exp->created_epoch;
}

int labtwin_experiment_history_json(const char *input, char *out,
                                    size_t out_size)
{
    static const char *const allowed[] = {
        "state", "query", "from_epoch", "to_epoch", "offset", "limit"
    };
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *state;
    cJSON *query;
    cJSON *from;
    cJSON *to;
    cJSON *offset_value;
    cJSON *limit_value;
    cJSON *data;
    cJSON *items;
    labtwin_experiment_t *exp;
    DIR *dir;
    struct dirent *entry;
    int offset = 0;
    int limit = 32;
    int total = 0;
    int returned = 0;
    int64_t from_epoch = 0;
    int64_t to_epoch = 0;

    if (!root || !fields_allowed(root, allowed, 6)) goto invalid;
    state = cJSON_GetObjectItem(root, "state");
    query = cJSON_GetObjectItem(root, "query");
    from = cJSON_GetObjectItem(root, "from_epoch");
    to = cJSON_GetObjectItem(root, "to_epoch");
    offset_value = cJSON_GetObjectItem(root, "offset");
    limit_value = cJSON_GetObjectItem(root, "limit");
    if ((state && !cJSON_IsString(state)) || (query && !cJSON_IsString(query)) ||
        (from && !cJSON_IsNumber(from)) || (to && !cJSON_IsNumber(to)) ||
        (offset_value && !cJSON_IsNumber(offset_value)) ||
        (limit_value && !cJSON_IsNumber(limit_value))) goto invalid;
    if (offset_value) offset = offset_value->valueint;
    if (limit_value) limit = limit_value->valueint;
    if (offset < 0 || limit < 1 || limit > 32) goto invalid;
    from_epoch = from ? (int64_t)from->valuedouble : 0;
    to_epoch = to ? (int64_t)to->valuedouble : 0;
    if (to_epoch && from_epoch && to_epoch < from_epoch) goto invalid;

    data = cJSON_CreateObject();
    items = cJSON_AddArrayToObject(data, "experiments");
    dir = opendir(LABTWIN_EXPERIMENTS);
    if (!dir) {
        cJSON_Delete(root);
        response(out, out_size, true, "OK", "experiment history empty", data);
        return 0;
    }
    /* labtwin_experiment_t is several KiB.  Keeping it in this REST call's
     * stack overflowed the per-client budget once routing and JSON frames
     * were included. */
    exp = malloc(sizeof(*exp));
    if (!exp) {
        closedir(dir);
        cJSON_Delete(root);
        cJSON_Delete(data);
        response(out, out_size, false, "MEMORY_UNAVAILABLE",
                 "experiment history memory unavailable", NULL);
        return -ENOMEM;
    }
    while ((entry = readdir(dir)) != NULL) {
        int64_t when;
        if (entry->d_name[0] == '.' || load_one(entry->d_name, exp) != 0)
            continue;
        when = calendar_epoch(exp);
        if ((state && strcmp(state->valuestring, labtwin_state_name(exp->state)) != 0) ||
            (query && !text_contains_ci(exp->name, query->valuestring) &&
             !text_contains_ci(exp->description, query->valuestring)) ||
            (from_epoch && (!when || when < from_epoch)) ||
            (to_epoch && (!when || when > to_epoch)))
            continue;
        if (total++ >= offset && returned < limit) {
            cJSON_AddItemToArray(items, experiment_json(exp));
            returned++;
        }
    }
    free(exp);
    closedir(dir);
    cJSON_AddNumberToObject(data, "count", total);
    cJSON_AddNumberToObject(data, "offset", offset);
    if (offset + returned < total) cJSON_AddNumberToObject(data, "next_offset", offset + returned);
    else cJSON_AddNullToObject(data, "next_offset");
    cJSON_Delete(root);
    response(out, out_size, true, "OK", "experiment history", data);
    return 0;
invalid:
    cJSON_Delete(root);
    response(out, out_size, false, "INVALID_ARGUMENT", "invalid history query", NULL);
    return -EINVAL;
}

int labtwin_experiment_update_json(const char *input, char *out,
                                   size_t out_size, const char *source)
{
    static const char *const allowed[] = {
        "experiment_id", "name", "description", "planned_start_epoch",
        "planned_end_epoch", "steps", "if_event_seq"
    };
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id;
    cJSON *name;
    cJSON *description;
    cJSON *steps;
    cJSON *sequence;
    cJSON *start;
    cJSON *end;
    cJSON *item;
    labtwin_experiment_t *exp;
    int i = 0;
    if (!root || !fields_allowed(root, allowed, 7) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32) ||
        !string_ok(name = cJSON_GetObjectItem(root, "name"), 64) ||
        !cJSON_IsArray(steps = cJSON_GetObjectItem(root, "steps")) ||
        cJSON_GetArraySize(steps) < 1 || cJSON_GetArraySize(steps) > LABTWIN_MAX_STEPS ||
        !cJSON_IsNumber(sequence = cJSON_GetObjectItem(root, "if_event_seq"))) goto invalid;
    description = cJSON_GetObjectItem(root, "description");
    start = cJSON_GetObjectItem(root, "planned_start_epoch");
    end = cJSON_GetObjectItem(root, "planned_end_epoch");
    if ((description && (!cJSON_IsString(description) ||
         strlen(description->valuestring) >= sizeof(exp->description))) ||
        (start && !cJSON_IsNumber(start) && !cJSON_IsNull(start)) ||
        (end && !cJSON_IsNumber(end) && !cJSON_IsNull(end)) ||
        (cJSON_IsNumber(start) && cJSON_IsNumber(end) &&
         start->valuedouble > 0 && end->valuedouble > 0 &&
         end->valuedouble < start->valuedouble)) goto invalid;
    pthread_mutex_lock(&g_lock);
    exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) {
        response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL);
        goto fail;
    }
    if (exp->state != LABTWIN_STATE_READY) {
        response(out, out_size, false, "INVALID_STATE", "started experiment structure is locked", NULL);
        goto fail;
    }
    if ((uint64_t)sequence->valuedouble != exp->last_event_seq) {
        response(out, out_size, false, "CONFLICT", "experiment changed; reload before saving", NULL);
        goto fail;
    }
    exp = stage(exp);
    snprintf(exp->name, sizeof(exp->name), "%s", name->valuestring);
    snprintf(exp->description, sizeof(exp->description), "%s",
             description ? description->valuestring : "");
    exp->planned_start_epoch = cJSON_IsNumber(start) && start->valuedouble > 0 ?
        (int64_t)start->valuedouble : 0;
    exp->planned_end_epoch = cJSON_IsNumber(end) && end->valuedouble > 0 ?
        (int64_t)end->valuedouble : 0;
    memset(exp->steps, 0, sizeof(exp->steps));
    cJSON_ArrayForEach(item, steps) {
        const char *title = cJSON_IsString(item) ? item->valuestring :
            cJSON_GetStringValue(cJSON_GetObjectItem(item, "title"));
        if (!title || !title[0] || strlen(title) >= sizeof(exp->steps[0].title)) {
            response(out, out_size, false, "INVALID_ARGUMENT", "invalid step title", NULL);
            goto fail;
        }
        snprintf(exp->steps[i++].title, sizeof(exp->steps[0].title), "%s", title);
    }
    exp->step_count = (uint8_t)i;
    exp->current_step = 0;
    if (commit(exp, "EXPERIMENT_UPDATED", source) != 0) {
        response(out, out_size, false, "STORAGE_ERROR", "experiment update could not be persisted", NULL);
        goto fail;
    }
    response(out, out_size, true, "OK", "experiment updated", view_json(exp));
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(root);
    return 0;
fail:
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(root);
    return -EINVAL;
invalid:
    response(out, out_size, false, "INVALID_ARGUMENT", "invalid experiment update", NULL);
    cJSON_Delete(root);
    return -EINVAL;
}

void labtwin_experiment_forget(const char *experiment_id)
{
    int i;
    if (!experiment_id) return;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        if (g_experiments[i].used && strcmp(g_experiments[i].id, experiment_id) == 0) {
            memset(&g_experiments[i], 0, sizeof(g_experiments[i]));
            if (g_focused == i) g_focused = -1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

int labtwin_experiment_delete_locked(const char *id, uint64_t expected_seq,
                                    int (*remove_files)(const char *, void *), void *context)
{
    int ret;
    labtwin_experiment_t *exp;
    if (!valid_id(id) || !remove_files) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    exp = find_or_load_experiment_locked(id);
    if (expected_seq && (!exp || exp->last_event_seq != expected_seq || exp->recovery_error)) ret = -EAGAIN;
    else {
        ret = remove_files(id, context);
        if (!ret && exp) {
            int slot = (int)(exp - g_experiments);
            memset(exp, 0, sizeof(*exp));
            if (g_focused == slot) g_focused = -1;
        } else if (ret && exp) {
            exp->recovery_error = true;
            exp->state = LABTWIN_STATE_RECOVERY_ERROR;
            snprintf(exp->recovery_reason, sizeof(exp->recovery_reason), "delete_incomplete");
            ret = -EUCLEAN;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

static int summary_compare(const void *left, const void *right)
{
    const labtwin_experiment_summary_t *a = left;
    const labtwin_experiment_summary_t *b = right;
    if (a->updated_epoch < b->updated_epoch) return 1;
    if (a->updated_epoch > b->updated_epoch) return -1;
    return strcmp(a->experiment_id, b->experiment_id);
}

int labtwin_experiment_list_summaries(labtwin_experiment_summary_t *items,
                                      size_t capacity, size_t *count)
{
    labtwin_experiment_summary_t all[LABTWIN_MAX_EXPERIMENTS];
    size_t total = 0;
    size_t copied;
    int i;

    if (!count || (capacity && !items)) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        labtwin_experiment_t *exp = &g_experiments[i];
        labtwin_experiment_summary_t *summary;
        int timer_index;
        if (!exp->used) continue;
        summary = &all[total++];
        memset(summary, 0, sizeof(*summary));
        snprintf(summary->experiment_id, sizeof(summary->experiment_id),
                 "%s", exp->id);
        snprintf(summary->name, sizeof(summary->name), "%s", exp->name);
        summary->state = exp->state;
        summary->current_step = exp->current_step;
        summary->step_count = exp->step_count;
        summary->updated_epoch = exp->updated_epoch;
        for (timer_index = 0; timer_index < exp->timer_count; timer_index++) {
            labtwin_timer_t *timer = &exp->timers[timer_index];
            if (timer->state == LABTWIN_TIMER_CANCELLED) continue;
            summary->has_timer = true;
            summary->timer_expired = timer->state == LABTWIN_TIMER_EXPIRED;
            summary->primary_remaining_seconds = summary->timer_expired ? -1 :
                                                  (int32_t)timer->remaining_seconds;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    qsort(all, total, sizeof(all[0]), summary_compare);
    copied = total < capacity ? total : capacity;
    if (copied) memcpy(items, all, copied * sizeof(items[0]));
    *count = copied;
    return 0;
}

int labtwin_experiment_snapshot_json(char *out, size_t out_size)
{
    cJSON *data = cJSON_CreateObject();
    cJSON *array;
    cJSON *root;
    char *text;
    size_t length;
    int i;

    if (!out || out_size == 0 || !data) {
        cJSON_Delete(data);
        return -EINVAL;
    }

    array = cJSON_AddArrayToObject(data, "experiments");
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        if (g_experiments[i].used)
            cJSON_AddItemToArray(array, experiment_json(&g_experiments[i]));
    }
    pthread_mutex_unlock(&g_lock);

    root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "code", "OK");
    cJSON_AddStringToObject(root, "message", "experiment snapshot");
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

static void pause_timers(labtwin_experiment_t *exp)
{
    int i; uint64_t now = monotonic_ms();
    for (i = 0; i < exp->timer_count; i++) if (exp->timers[i].state == LABTWIN_TIMER_RUNNING) {
        exp->timers[i].remaining_seconds = exp->timers[i].deadline_ms > now ?
            (uint32_t)((exp->timers[i].deadline_ms - now + 999) / 1000) : 0;
        exp->timers[i].state = LABTWIN_TIMER_PAUSED;
    }
}

static void resume_timers(labtwin_experiment_t *exp)
{
    int i; uint64_t now = monotonic_ms(); int64_t epoch = wall_epoch();
    for (i = 0; i < exp->timer_count; i++) if (exp->timers[i].state == LABTWIN_TIMER_PAUSED) {
        exp->timers[i].deadline_ms = now + (uint64_t)exp->timers[i].remaining_seconds * 1000ULL;
        exp->timers[i].due_epoch = labtwin_clock_trusted() ? epoch + exp->timers[i].remaining_seconds : 0;
        exp->timers[i].state = LABTWIN_TIMER_RUNNING;
    }
}

static void cancel_timers(labtwin_experiment_t *exp)
{
    int i;
    for (i = 0; i < exp->timer_count; i++)
        if (exp->timers[i].state == LABTWIN_TIMER_RUNNING ||
            exp->timers[i].state == LABTWIN_TIMER_PAUSED)
            exp->timers[i].state = LABTWIN_TIMER_CANCELLED;
}

int labtwin_experiment_transition_json(const char *input, char *out,
                                       size_t out_size, const char *source)
{
    static const char *const allowed[] = {"experiment_id", "action", "confirmation", "if_event_seq"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id, *action;
    labtwin_experiment_t *exp;
    const char *event = NULL;
    if (!root || !fields_allowed(root, allowed, 4) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32) ||
        !string_ok(action = cJSON_GetObjectItem(root, "action"), 32)) goto invalid;
    pthread_mutex_lock(&g_lock); exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) { response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL); goto fail_unlock; }
    cJSON *expected = cJSON_GetObjectItem(root, "if_event_seq");
    if (expected && (!cJSON_IsNumber(expected) || expected->valuedouble != (double)exp->last_event_seq)) {
        response(out, out_size, false, "CONFLICT", "experiment changed; prepare a new operation", NULL);
        goto fail_unlock;
    }
    if (exp->recovery_error || exp->state == LABTWIN_STATE_COMPLETED || exp->state == LABTWIN_STATE_CANCELLED) {
        response(out, out_size, false, "INVALID_STATE", "experiment is not mutable", NULL); goto fail_unlock;
    }
    exp = stage(exp);
    if (!strcmp(action->valuestring, "start") && exp->state == LABTWIN_STATE_READY) {
        exp->state = LABTWIN_STATE_RUNNING;
        exp->started_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
        event = "EXPERIMENT_STARTED";
    } else if (!strcmp(action->valuestring, "pause") && exp->state == LABTWIN_STATE_RUNNING) {
        pause_timers(exp); exp->state = LABTWIN_STATE_PAUSED; event = "EXPERIMENT_PAUSED";
    } else if (!strcmp(action->valuestring, "resume") && exp->state == LABTWIN_STATE_PAUSED) {
        resume_timers(exp); exp->state = LABTWIN_STATE_RUNNING; exp->recovery_reason[0] = 0; event = "EXPERIMENT_RESUMED";
    } else if (!strcmp(action->valuestring, "complete_step") && exp->state == LABTWIN_STATE_RUNNING && exp->current_step + 1 < exp->step_count) {
        exp->steps[exp->current_step].completed = true; exp->current_step++; event = "STEP_COMPLETED";
    } else if (!strcmp(action->valuestring, "complete") && exp->state == LABTWIN_STATE_RUNNING && exp->current_step + 1 == exp->step_count) {
        exp->steps[exp->current_step].completed = true; cancel_timers(exp);
        exp->state = LABTWIN_STATE_COMPLETED;
        exp->ended_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
        event = "EXPERIMENT_COMPLETED";
    } else if (!strcmp(action->valuestring, "cancel")) {
        cancel_timers(exp); exp->state = LABTWIN_STATE_CANCELLED;
        exp->ended_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
        event = "EXPERIMENT_CANCELLED";
    } else {
        response(out, out_size, false, "INVALID_STATE", "transition is not allowed", NULL); goto fail_unlock;
    }
    int persist_ret = commit(exp, event, source);
    if (persist_ret != 0) {
        response(out, out_size, false, persist_ret == -EUCLEAN ? "RECOVERY_ERROR" : "STORAGE_ERROR",
                 persist_ret == -EUCLEAN ? "journal outcome uncertain; task isolated; do not retry" : "transition could not be persisted", NULL);
        pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return persist_ret;
    }
    response(out, out_size, true, "OK", "transition applied", view_json(exp));
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return 0;
fail_unlock:
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return -EINVAL;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid transition input", NULL); return -EINVAL;
}

int labtwin_timer_start_json(const char *input, char *out, size_t out_size,
                             const char *source)
{
    static const char *const allowed[] = {"experiment_id", "step_index", "label", "duration_seconds"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id, *step, *label, *duration;
    labtwin_experiment_t *exp;
    labtwin_timer_t *timer;
    int active = 0, i;
    if (!root || !fields_allowed(root, allowed, 4) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32) ||
        !cJSON_IsNumber(step = cJSON_GetObjectItem(root, "step_index")) ||
        !string_ok(label = cJSON_GetObjectItem(root, "label"), 64) ||
        !cJSON_IsNumber(duration = cJSON_GetObjectItem(root, "duration_seconds")) ||
        duration->valuedouble < 1 || duration->valuedouble > LABTWIN_MAX_DURATION) goto invalid;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) if (g_experiments[i].used) {
        int j; for (j = 0; j < g_experiments[i].timer_count; j++)
            if (g_experiments[i].timers[j].state == LABTWIN_TIMER_RUNNING ||
                g_experiments[i].timers[j].state == LABTWIN_TIMER_PAUSED) active++;
    }
    exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) { response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL); goto fail; }
    if (exp->state != LABTWIN_STATE_RUNNING || step->valueint < 0 || step->valueint >= exp->step_count) {
        response(out, out_size, false, "INVALID_STATE", "timer requires a running experiment and valid step", NULL); goto fail;
    }
    if (active >= LABTWIN_MAX_TIMERS) {
        response(out, out_size, false, "LIMIT_REACHED", "timer limit reached", NULL); goto fail;
    }
    exp = stage(exp);
    for (i = 0; i < exp->timer_count; i++)
        if (exp->timers[i].state == LABTWIN_TIMER_CANCELLED ||
            exp->timers[i].state == LABTWIN_TIMER_EXPIRED) break;
    if (i == exp->timer_count) {
        if (exp->timer_count >= LABTWIN_MAX_TIMERS) {
            response(out, out_size, false, "LIMIT_REACHED", "experiment timer slots exhausted", NULL); goto fail;
        }
        exp->timer_count++;
    }
    timer = &exp->timers[i]; memset(timer, 0, sizeof(*timer));
    snprintf(timer->id, sizeof(timer->id), "T-%08lx", (unsigned long)(monotonic_ms() + g_id_counter++));
    snprintf(timer->label, sizeof(timer->label), "%s", label->valuestring);
    timer->step_index = (uint8_t)step->valueint;
    timer->remaining_seconds = (uint32_t)duration->valueint;
    timer->deadline_ms = monotonic_ms() + (uint64_t)timer->remaining_seconds * 1000ULL;
    timer->due_epoch = labtwin_clock_trusted() ? wall_epoch() + timer->remaining_seconds : 0;
    timer->state = LABTWIN_TIMER_RUNNING;
    if (commit(exp, "TIMER_STARTED", source) != 0) {
        memset(timer, 0, sizeof(*timer)); response(out, out_size, false, "STORAGE_ERROR", "timer could not be persisted", NULL); goto fail;
    }
    response(out, out_size, true, "OK", "timer started", view_json(exp));
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return 0;
fail:
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return -EINVAL;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid timer input", NULL); return -EINVAL;
}

int labtwin_timer_cancel_json(const char *input, char *out, size_t out_size,
                              const char *source)
{
    static const char *const allowed[] = {"experiment_id", "timer_id", "reason"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id, *timer_id;
    labtwin_experiment_t *exp;
    int i;
    if (!root || !fields_allowed(root, allowed, 3) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32) ||
        !string_ok(timer_id = cJSON_GetObjectItem(root, "timer_id"), 16)) goto invalid;
    pthread_mutex_lock(&g_lock); exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) { response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL); goto fail; }
    for (i = 0; i < exp->timer_count; i++) if (!strcmp(exp->timers[i].id, timer_id->valuestring)) break;
    if (i == exp->timer_count) { response(out, out_size, false, "NOT_FOUND", "timer not found", NULL); goto fail; }
    if (exp->timers[i].state == LABTWIN_TIMER_CANCELLED || exp->timers[i].state == LABTWIN_TIMER_EXPIRED) {
        response(out, out_size, false, "INVALID_STATE", "timer is already final", NULL); goto fail;
    }
    exp = stage(exp);
    exp->timers[i].state = LABTWIN_TIMER_CANCELLED;
    if (commit(exp, "TIMER_CANCELLED", source) != 0) { response(out, out_size, false, "STORAGE_ERROR", "timer cancel not persisted", NULL); goto fail; }
    response(out, out_size, true, "OK", "timer cancelled", view_json(exp));
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return 0;
fail:
    pthread_mutex_unlock(&g_lock); cJSON_Delete(root); return -EINVAL;
invalid:
    cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid timer cancel input", NULL); return -EINVAL;
}

int labtwin_log_add_json(const char *input, char *out, size_t out_size,
                         const char *source)
{
    static const char *const allowed[] = {"experiment_id", "text", "structured"};
    cJSON *root = cJSON_Parse(input ? input : "{}");
    cJSON *id, *text, *structured;
    char *structured_text = NULL;
    labtwin_experiment_t *exp;
    if (!root || !fields_allowed(root, allowed, 3) ||
        !string_ok(id = cJSON_GetObjectItem(root, "experiment_id"), 32) ||
        !string_ok(text = cJSON_GetObjectItem(root, "text"), 513)) goto invalid;
    structured = cJSON_GetObjectItem(root, "structured");
    if (structured && !cJSON_IsObject(structured)) goto invalid;
    if (structured) {
        structured_text = cJSON_PrintUnformatted(structured);
        if (!structured_text || strlen(structured_text) >= 1025) goto invalid;
    }
    pthread_mutex_lock(&g_lock); exp = find_or_load_experiment_locked(id->valuestring);
    if (!exp) { response(out, out_size, false, "NOT_FOUND", "experiment not found", NULL); goto fail; }
    if (exp->state == LABTWIN_STATE_COMPLETED || exp->state == LABTWIN_STATE_CANCELLED || exp->recovery_error) {
        response(out, out_size, false, "INVALID_STATE", "experiment is not mutable", NULL); goto fail;
    }
    if (strcmp(exp->last_observation, text->valuestring) == 0) {
        response(out, out_size, true, "ALREADY_APPLIED",
                 "identical observation already recorded", view_json(exp));
        pthread_mutex_unlock(&g_lock); free(structured_text); cJSON_Delete(root); return 0;
    }
    exp = stage(exp);
    snprintf(exp->last_observation, sizeof(exp->last_observation), "%s",
             text->valuestring);
    snprintf(exp->last_observation_structured,
             sizeof(exp->last_observation_structured), "%s",
             structured_text ? structured_text : "{}");
    if (commit(exp, "OBSERVATION_ADDED", source) != 0) {
        response(out, out_size, false, "STORAGE_ERROR", "observation not persisted", NULL); goto fail;
    }
    response(out, out_size, true, "OK", "observation recorded", view_json(exp));
    pthread_mutex_unlock(&g_lock); free(structured_text); cJSON_Delete(root); return 0;
fail:
    pthread_mutex_unlock(&g_lock); free(structured_text); cJSON_Delete(root); return -EINVAL;
invalid:
    free(structured_text); cJSON_Delete(root); response(out, out_size, false, "INVALID_ARGUMENT", "invalid observation input", NULL); return -EINVAL;
}

void labtwin_sensor_update(bool tv, float temp, bool hv, float hum,
                           bool pv, float prox)
{
    uint64_t now_ms = monotonic_ms();
    pthread_mutex_lock(&g_lock);
    if (tv) {
        if (isfinite(temp) && temp >= -40.0f && temp <= 125.0f) {
            g_sensors.temperature_available = true;
            g_sensors.temperature_c = temp;
            g_sensors.temperature_sampled_ms = now_ms;
        } else {
            g_sensors.temperature_available = false;
            g_sensors.temperature_failures++;
        }
    }
    if (hv) {
        if (isfinite(hum) && hum >= 0.0f && hum <= 100.0f) {
            g_sensors.humidity_available = true;
            g_sensors.humidity_percent = hum;
            g_sensors.humidity_sampled_ms = now_ms;
        } else {
            g_sensors.humidity_available = false;
            g_sensors.humidity_failures++;
        }
    }
    if (pv) { g_sensors.proximity_available = true; g_sensors.proximity_cm = prox; }
    g_sensors.sampled_epoch = labtwin_clock_trusted() ? wall_epoch() : 0;
    pthread_mutex_unlock(&g_lock);
    labtwin_environment_submit(tv, temp, hv, hum);
}

void labtwin_sensor_failure(bool temperature_failed, bool humidity_failed)
{
    pthread_mutex_lock(&g_lock);
    if (temperature_failed) {
        g_sensors.temperature_failures++;
        g_sensors.temperature_available = false;
    }
    if (humidity_failed) {
        g_sensors.humidity_failures++;
        g_sensors.humidity_available = false;
    }
    pthread_mutex_unlock(&g_lock);
    labtwin_environment_submit(temperature_failed, NAN,
                               humidity_failed, NAN);
}

int labtwin_sensor_snapshot_json(char *out, size_t out_size)
{
    cJSON *data = cJSON_CreateObject();
    labtwin_sensor_snapshot_t snapshot;
    labtwin_sensor_get(&snapshot);
    cJSON_AddBoolToObject(data, "temperature_available", snapshot.temperature_available);
    cJSON_AddNumberToObject(data, "temperature_c", snapshot.temperature_c);
    cJSON_AddNumberToObject(data, "temperature_age_ms", snapshot.temperature_age_ms);
    cJSON_AddNumberToObject(data, "temperature_failures", snapshot.temperature_failures);
    cJSON_AddBoolToObject(data, "humidity_available", snapshot.humidity_available);
    cJSON_AddNumberToObject(data, "humidity_percent", snapshot.humidity_percent);
    cJSON_AddNumberToObject(data, "humidity_age_ms", snapshot.humidity_age_ms);
    cJSON_AddNumberToObject(data, "humidity_failures", snapshot.humidity_failures);
    cJSON_AddBoolToObject(data, "proximity_available", snapshot.proximity_available);
    cJSON_AddNumberToObject(data, "proximity_cm", snapshot.proximity_cm);
    cJSON_AddNumberToObject(data, "sampled_epoch", (double)snapshot.sampled_epoch);
    response(out, out_size, true, "OK", "sensor snapshot", data);
    return 0;
}

int labtwin_sensor_get(labtwin_sensor_snapshot_t *snapshot)
{
    uint64_t now = monotonic_ms();
    if (!snapshot) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    *snapshot = g_sensors;
    snapshot->temperature_age_ms = snapshot->temperature_sampled_ms &&
        now >= snapshot->temperature_sampled_ms ?
        (uint32_t)(now - snapshot->temperature_sampled_ms) : UINT32_MAX;
    snapshot->humidity_age_ms = snapshot->humidity_sampled_ms &&
        now >= snapshot->humidity_sampled_ms ?
        (uint32_t)(now - snapshot->humidity_sampled_ms) : UINT32_MAX;
    if (snapshot->temperature_age_ms > 5000) snapshot->temperature_available = false;
    if (snapshot->humidity_age_ms > 5000) snapshot->humidity_available = false;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int labtwin_environment_resolve_experiment(const char *preferred,
                                           char *output,
                                           size_t output_size)
{
    int i;
    int count = 0;
    labtwin_experiment_t *only = NULL;
    if (!output || output_size == 0) return -EINVAL;
    output[0] = 0;
    pthread_mutex_lock(&g_lock);
    if (preferred && preferred[0]) {
        labtwin_experiment_t *exp = find_experiment(preferred);
        if (exp && (exp->state == LABTWIN_STATE_RUNNING ||
                    exp->state == LABTWIN_STATE_PAUSED)) {
            snprintf(output, output_size, "%s", exp->id);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        labtwin_experiment_t *exp = &g_experiments[i];
        if (exp->used && (exp->state == LABTWIN_STATE_RUNNING ||
                         exp->state == LABTWIN_STATE_PAUSED)) {
            only = exp;
            count++;
        }
    }
    if (count == 1 && only) snprintf(output, output_size, "%s", only->id);
    pthread_mutex_unlock(&g_lock);
    return count == 1 ? 0 : -ENOENT;
}

int labtwin_get_focused_view(labtwin_experiment_view_t *view)
{
    labtwin_experiment_t *exp;
    int i, count = 0;
    if (!view) return -EINVAL;
    pthread_mutex_lock(&g_lock); memset(view, 0, sizeof(*view));
    if (g_focused < 0 || !g_experiments[g_focused].used) {
        pthread_mutex_unlock(&g_lock); return -ENOENT;
    }
    exp = &g_experiments[g_focused];
    snprintf(view->experiment_id, sizeof(view->experiment_id), "%s", exp->id);
    snprintf(view->name, sizeof(view->name), "%s", exp->name);
    snprintf(view->step, sizeof(view->step), "%s", exp->steps[exp->current_step].title);
    snprintf(view->recovery_reason, sizeof(view->recovery_reason), "%s", exp->recovery_reason);
    view->state = exp->state; view->step_index = exp->current_step;
    view->step_count = exp->step_count;
    for (i = 0; i < exp->timer_count && count < LABTWIN_UI_TIMERS; i++) {
        labtwin_timer_t *t = &exp->timers[i];
        if (t->state == LABTWIN_TIMER_CANCELLED) continue;
        snprintf(view->timers[count].id, sizeof(view->timers[count].id), "%s", t->id);
        snprintf(view->timers[count].label, sizeof(view->timers[count].label), "%s", t->label);
        view->timers[count].remaining_seconds = t->remaining_seconds;
        view->timers[count].step_index = t->step_index;
        view->timers[count].state = t->state; count++;
    }
    view->timer_count = count;
    pthread_mutex_unlock(&g_lock); return 0;
}

int labtwin_focus_next(void)
{
    int n;
    pthread_mutex_lock(&g_lock);
    for (n = 1; n <= LABTWIN_MAX_EXPERIMENTS; n++) {
        int idx = (g_focused + n + LABTWIN_MAX_EXPERIMENTS) % LABTWIN_MAX_EXPERIMENTS;
        if (g_experiments[idx].used &&
            (g_experiments[idx].state == LABTWIN_STATE_RUNNING ||
             g_experiments[idx].state == LABTWIN_STATE_PAUSED ||
             g_experiments[idx].state == LABTWIN_STATE_RECOVERY_ERROR)) {
            g_focused = idx; pthread_mutex_unlock(&g_lock); return 0;
        }
    }
    pthread_mutex_unlock(&g_lock); return -ENOENT;
}

int labtwin_focus_experiment(const char *experiment_id)
{
    int i;
    if (!experiment_id || !experiment_id[0]) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < LABTWIN_MAX_EXPERIMENTS; i++) {
        labtwin_experiment_t *exp = &g_experiments[i];
        if (!exp->used || strcmp(exp->id, experiment_id) != 0) continue;
        if (exp->state == LABTWIN_STATE_COMPLETED ||
            exp->state == LABTWIN_STATE_CANCELLED) {
            pthread_mutex_unlock(&g_lock);
            return -EINVAL;
        }
        g_focused = i;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_lock);
    return -ENOENT;
}

int labtwin_ui_transition(const char *action)
{
    char input[128], output[512];
    labtwin_experiment_view_t view;
    if (labtwin_get_focused_view(&view) != 0) return -ENOENT;
    snprintf(input, sizeof(input), "{\"experiment_id\":\"%s\",\"action\":\"%s\"}",
             view.experiment_id, action);
    return labtwin_experiment_transition_json(input, output, sizeof(output), "ui");
}
