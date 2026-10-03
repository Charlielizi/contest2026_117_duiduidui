#include "voice/voice_request_ledger.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEDGER_MAGIC 0x564C4544u
#define LEDGER_VERSION 1u
#define LEDGER_CAPACITY 64u
#define LEDGER_DEFAULT_PATH "/data/ai_agent/voice_request_ledger.bin"

typedef struct {
    char request_id[40];
    char tool[32];
    char result[768];
    uint32_t arguments_hash;
    uint32_t transcript_hash;
    uint32_t sequence;
    uint8_t state;
    uint8_t reserved[3];
} ledger_record_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t capacity;
    uint32_t next_sequence;
    uint32_t checksum;
    ledger_record_t records[LEDGER_CAPACITY];
} ledger_file_t;

static pthread_mutex_t g_ledger_lock = PTHREAD_MUTEX_INITIALIZER;
static ledger_file_t g_ledger;
static char g_path[160] = LEDGER_DEFAULT_PATH;
static int g_loaded;
static int g_load_status;
#ifndef __NuttX__
static int g_fail_persist_after = -1;
#endif

static uint32_t checksum_bytes(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static uint32_t ledger_checksum(const ledger_file_t *ledger)
{
    ledger_file_t copy = *ledger;
    copy.checksum = 0;
    return checksum_bytes(&copy, sizeof(copy));
}

static void ledger_empty(void)
{
    memset(&g_ledger, 0, sizeof(g_ledger));
    g_ledger.magic = LEDGER_MAGIC;
    g_ledger.version = LEDGER_VERSION;
    g_ledger.capacity = LEDGER_CAPACITY;
    g_ledger.next_sequence = 1;
}

static int write_all(int fd, const void *data, size_t size)
{
    const uint8_t *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0) return -errno;
        if (n == 0) return -EIO;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int persist_locked(void)
{
#ifndef __NuttX__
    if (g_fail_persist_after == 0) {
        g_fail_persist_after = -1;
        return -EIO;
    }
    if (g_fail_persist_after > 0)
        g_fail_persist_after--;
#endif
    char tmp[176];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    g_ledger.checksum = ledger_checksum(&g_ledger);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    int ret = write_all(fd, &g_ledger, sizeof(g_ledger));
#ifndef __NuttX__
    if (!ret && fsync(fd) < 0 && errno != ENOSYS) ret = -errno;
#endif
    if (close(fd) < 0 && !ret) ret = -errno;
    if (!ret && rename(tmp, g_path) < 0) ret = -errno;
    if (ret) unlink(tmp);
    return ret;
}

static int load_locked(void)
{
    if (g_loaded) return g_load_status;
    ledger_empty();
    int fd = open(g_path, O_RDONLY);
    if (fd >= 0) {
        ledger_file_t disk;
        ssize_t n = read(fd, &disk, sizeof(disk));
        close(fd);
        if (n == (ssize_t)sizeof(disk) && disk.magic == LEDGER_MAGIC &&
            disk.version == LEDGER_VERSION &&
            disk.capacity == LEDGER_CAPACITY &&
            disk.checksum == ledger_checksum(&disk)) {
            g_ledger = disk;
            g_load_status = 0;
        } else {
            /* A corrupt exactly-once ledger must fail closed.  Treating it as
             * empty would allow previously applied requests to run again. */
            g_load_status = -EIO;
        }
    } else if (errno == ENOENT) {
        g_load_status = 0;
    } else {
        g_load_status = -errno;
    }
    g_loaded = 1;
    return g_load_status;
}

static ledger_record_t *find_locked(const char *request_id)
{
    for (size_t i = 0; i < LEDGER_CAPACITY; i++)
        if (g_ledger.records[i].request_id[0] &&
            strcmp(g_ledger.records[i].request_id, request_id) == 0)
            return &g_ledger.records[i];
    return NULL;
}

int voice_request_ledger_init(const char *path)
{
    pthread_mutex_lock(&g_ledger_lock);
    if (path && path[0]) snprintf(g_path, sizeof(g_path), "%s", path);
    g_loaded = 0;
    g_load_status = 0;
    int ret = load_locked();
    pthread_mutex_unlock(&g_ledger_lock);
    return ret;
}

int voice_request_ledger_prepare(const char *request_id, const char *tool,
                                 const char *arguments,
                                 const char *transcript)
{
    if (!request_id || !request_id[0] || !tool || !arguments || !transcript)
        return -EINVAL;
    pthread_mutex_lock(&g_ledger_lock);
    int load_ret = load_locked();
    if (load_ret != 0) {
        pthread_mutex_unlock(&g_ledger_lock);
        return load_ret;
    }
    uint32_t arguments_hash = checksum_bytes(arguments, strlen(arguments));
    uint32_t transcript_hash = checksum_bytes(transcript, strlen(transcript));
    ledger_record_t *old = find_locked(request_id);
    if (old) {
        int same = strcmp(old->tool, tool) == 0 &&
                   old->arguments_hash == arguments_hash &&
                   old->transcript_hash == transcript_hash;
        pthread_mutex_unlock(&g_ledger_lock);
        return same ? 1 : -EEXIST;
    }
    size_t slot = LEDGER_CAPACITY;
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < LEDGER_CAPACITY; i++) {
        if (!g_ledger.records[i].request_id[0]) { slot = i; oldest = 0; break; }
        /* Never evict an action that is awaiting confirmation or whose
         * execution outcome is uncertain. */
        if (g_ledger.records[i].state != VOICE_LEDGER_PENDING &&
            g_ledger.records[i].state != VOICE_LEDGER_EXECUTING &&
            g_ledger.records[i].sequence < oldest) {
            oldest = g_ledger.records[i].sequence;
            slot = i;
        }
    }
    if (slot == LEDGER_CAPACITY) {
        pthread_mutex_unlock(&g_ledger_lock);
        return -ENOSPC;
    }
    ledger_record_t previous = g_ledger.records[slot];
    uint32_t previous_sequence = g_ledger.next_sequence;
    ledger_record_t *r = &g_ledger.records[slot];
    memset(r, 0, sizeof(*r));
    snprintf(r->request_id, sizeof(r->request_id), "%s", request_id);
    snprintf(r->tool, sizeof(r->tool), "%s", tool);
    r->arguments_hash = arguments_hash;
    r->transcript_hash = transcript_hash;
    r->state = VOICE_LEDGER_PENDING;
    r->sequence = g_ledger.next_sequence++;
    int ret = persist_locked();
    if (ret != 0) {
        *r = previous;
        g_ledger.next_sequence = previous_sequence;
    }
    pthread_mutex_unlock(&g_ledger_lock);
    return ret;
}

int voice_request_ledger_lookup(const char *request_id,
                                voice_ledger_state_t *state,
                                char *result, size_t result_size)
{
    if (!request_id || !request_id[0]) return -EINVAL;
    pthread_mutex_lock(&g_ledger_lock);
    int load_ret = load_locked();
    if (load_ret != 0) {
        pthread_mutex_unlock(&g_ledger_lock);
        return load_ret;
    }
    ledger_record_t *r = find_locked(request_id);
    if (!r) { pthread_mutex_unlock(&g_ledger_lock); return -ENOENT; }
    if (state) *state = (voice_ledger_state_t)r->state;
    if (result && result_size) snprintf(result, result_size, "%s", r->result);
    pthread_mutex_unlock(&g_ledger_lock);
    return 0;
}

int voice_request_ledger_finish(const char *request_id,
                                voice_ledger_state_t state,
                                const char *result)
{
    if (!request_id || state < VOICE_LEDGER_PENDING ||
        state > VOICE_LEDGER_EXECUTING) return -EINVAL;
    pthread_mutex_lock(&g_ledger_lock);
    int load_ret = load_locked();
    if (load_ret != 0) {
        pthread_mutex_unlock(&g_ledger_lock);
        return load_ret;
    }
    ledger_record_t *r = find_locked(request_id);
    if (!r) { pthread_mutex_unlock(&g_ledger_lock); return -ENOENT; }
    if (r->state == VOICE_LEDGER_APPLIED && state != VOICE_LEDGER_APPLIED) {
        pthread_mutex_unlock(&g_ledger_lock);
        return -EALREADY;
    }
    uint8_t old_state = r->state;
    char old_result[sizeof(r->result)];
    snprintf(old_result, sizeof(old_result), "%s", r->result);
    r->state = (uint8_t)state;
    snprintf(r->result, sizeof(r->result), "%s", result ? result : "");
    int ret = persist_locked();
    if (ret != 0) {
        r->state = old_state;
        snprintf(r->result, sizeof(r->result), "%s", old_result);
    }
    pthread_mutex_unlock(&g_ledger_lock);
    return ret;
}

void voice_request_ledger_reset_for_test(void)
{
    pthread_mutex_lock(&g_ledger_lock);
    g_loaded = 0;
    g_load_status = 0;
    ledger_empty();
    pthread_mutex_unlock(&g_ledger_lock);
}

#ifndef __NuttX__
void voice_request_ledger_fail_persist_after_for_test(unsigned int successes)
{
    pthread_mutex_lock(&g_ledger_lock);
    g_fail_persist_after = (int)successes;
    pthread_mutex_unlock(&g_ledger_lock);
}
#endif
