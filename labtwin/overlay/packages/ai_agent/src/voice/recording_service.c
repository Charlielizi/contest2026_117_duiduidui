#include "voice/recording_service.h"

#ifndef RECORDING_HOST_TEST
#include "agent_config.h"
#else
#define AGENT_AUDIO_CAPTURE_DEV "default"
#endif
#include "voice/audio_arbiter.h"
#include "voice/audio_capture.h"
#ifdef CONFIG_AI_AGENT_WAKEWORD
#include "voice/wakeword_engine.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <syslog.h>
#include <unistd.h>

#include "cJSON.h"

#define TAG "recording"
#ifndef RECORDINGS_ROOT
#define RECORDINGS_ROOT "/data/labtwin/recordings"
#endif
#ifndef RECORDINGS_PARENT
#define RECORDINGS_PARENT "/data/labtwin"
#endif
#define RECORDING_ID_MAX 16
#define RECORDING_PATH_MAX 320
#define WAV_HEADER_BYTES 44U
#define RECORDING_RATE 16000U
#define RECORDING_CHANNELS 1U
#define RECORDING_BITS 16U
#define RECORDING_BYTES_PER_SECOND 32000ULL
#define RECORDING_MAX_SECONDS 3600ULL
#define RECORDING_MAX_DATA_BYTES (RECORDING_BYTES_PER_SECOND * RECORDING_MAX_SECONDS)
#define RECORDING_QUOTA_BYTES (120ULL * 1024ULL * 1024ULL)
#define RECORDING_FS_RESERVE_BYTES (16ULL * 1024ULL * 1024ULL)
#define RECORDING_MIN_SECONDS 1ULL
#define RECORDING_CHUNK_BYTES 4096U
#define RECORDING_SYNC_BYTES (RECORDING_BYTES_PER_SECOND * 60ULL)

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    enum { REC_IDLE, REC_STARTING, REC_RECORDING, REC_STOPPING, REC_FINALIZING } phase;
    bool initialized;
    bool active;
    bool stop_requested;
    bool thread_started;
    bool joining;
    bool resume_wake;
    bool published;
    pthread_t thread;
    audio_capture_t *capture;
    int fd;
    char id[RECORDING_ID_MAX];
    char part_path[RECORDING_PATH_MAX];
    char final_path[RECORDING_PATH_MAX];
    unsigned long long data_bytes;
    unsigned long long max_data_bytes;
    int last_error;
    char end_reason[32];
} recording_state_t;

static recording_state_t g_recording = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
    .fd = -1,
};

static bool ends_with(const char *value, const char *suffix)
{
    size_t value_len = value ? strlen(value) : 0;
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool valid_id(const char *id)
{
    size_t i;
    if (!id || strncmp(id, "rec-", 4) != 0 || strlen(id) != 10)
        return false;
    for (i = 4; i < 10; i++)
        if (id[i] < '0' || id[i] > '9') return false;
    return true;
}

static int mkdir_checked(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -errno;
    return 0;
}

static void put_le16(unsigned char *out, unsigned int value)
{
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8) & 0xffU);
}

static void put_le32(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8) & 0xffU);
    out[2] = (unsigned char)((value >> 16) & 0xffU);
    out[3] = (unsigned char)((value >> 24) & 0xffU);
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    while (length) {
        ssize_t written = write(fd, cursor, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 ? -errno : -EIO;
        cursor += written;
        length -= (size_t)written;
    }
    return 0;
}

static int write_wav_header(int fd, unsigned long long data_bytes)
{
    unsigned char header[WAV_HEADER_BYTES] = {0};
    uint32_t data = data_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)data_bytes;
    if (lseek(fd, 0, SEEK_SET) < 0) return -errno;
    memcpy(header, "RIFF", 4);
    put_le32(header + 4, 36U + data);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);
    put_le16(header + 22, RECORDING_CHANNELS);
    put_le32(header + 24, RECORDING_RATE);
    put_le32(header + 28, (uint32_t)RECORDING_BYTES_PER_SECOND);
    put_le16(header + 32, 2);
    put_le16(header + 34, RECORDING_BITS);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, data);
    if (write_all(fd, header, sizeof(header)) < 0) return -EIO;
    if (lseek(fd, 0, SEEK_END) < 0) return -errno;
    return 0;
}

static unsigned long long recording_bytes_used(void)
{
    DIR *dir = opendir(RECORDINGS_ROOT);
    struct dirent *entry;
    unsigned long long used = 0;
    if (!dir) return 0;
    while ((entry = readdir(dir)) != NULL) {
        char path[RECORDING_PATH_MAX];
        struct stat st;
        if (!ends_with(entry->d_name, ".wav") &&
            !ends_with(entry->d_name, ".wav.part")) continue;
        snprintf(path, sizeof(path), "%s/%s", RECORDINGS_ROOT, entry->d_name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            used += (unsigned long long)st.st_size;
    }
    closedir(dir);
    return used;
}

static unsigned long long recording_free_bytes(void)
{
    struct statfs fs;
    if (statfs(RECORDINGS_ROOT, &fs) != 0) return 0;
    return (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_bsize;
}

static unsigned long long permitted_data_bytes(void)
{
    unsigned long long used = recording_bytes_used();
    unsigned long long free_bytes = recording_free_bytes();
    unsigned long long quota_left = used < RECORDING_QUOTA_BYTES ?
        RECORDING_QUOTA_BYTES - used : 0;
    unsigned long long fs_left = free_bytes > RECORDING_FS_RESERVE_BYTES ?
        free_bytes - RECORDING_FS_RESERVE_BYTES : 0;
    unsigned long long allowed = quota_left < fs_left ? quota_left : fs_left;
    if (allowed > WAV_HEADER_BYTES) allowed -= WAV_HEADER_BYTES;
    else allowed = 0;
    return allowed < RECORDING_MAX_DATA_BYTES ? allowed : RECORDING_MAX_DATA_BYTES;
}

static unsigned int next_recording_number(void)
{
    DIR *dir = opendir(RECORDINGS_ROOT);
    struct dirent *entry;
    unsigned int next = 1;
    if (!dir) return next;
    while ((entry = readdir(dir)) != NULL) {
        unsigned int value = 0;
        if (sscanf(entry->d_name, "rec-%u.wav", &value) == 1 && value >= next)
            next = value + 1;
    }
    closedir(dir);
    return next;
}

static void resume_wakeword(void)
{
#ifdef CONFIG_AI_AGENT_WAKEWORD
    if (g_recording.resume_wake) wakeword_engine_resume();
#endif
}

static void pause_wakeword(void)
{
#ifdef CONFIG_AI_AGENT_WAKEWORD
    g_recording.resume_wake = wakeword_engine_is_listening();
    wakeword_engine_pause();
#endif
}

static int acquire_recording_audio(void)
{
    int attempt;
    pause_wakeword();
    for (attempt = 0; attempt < 50; attempt++) {
        if (audio_arbiter_acquire(AUDIO_OWNER_RECORDING) == 0)
            return 0;
        usleep(20 * 1000);
    }
    resume_wakeword();
    return -EBUSY;
}

static void repair_part_file(const char *name)
{
    char part[RECORDING_PATH_MAX];
    char final[RECORDING_PATH_MAX];
    struct stat st;
    int fd;
    size_t length = strlen(name);
    unsigned long long data_bytes;

    if (!ends_with(name, ".wav.part") || length < 14) return;
    snprintf(part, sizeof(part), "%s/%s", RECORDINGS_ROOT, name);
    if (stat(part, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (st.st_size < WAV_HEADER_BYTES) {
        g_recording.last_error = -EIO; /* Preserve unrepairable input for inspection. */
        return;
    }
    data_bytes = (unsigned long long)st.st_size - WAV_HEADER_BYTES;
    if (data_bytes > RECORDING_MAX_DATA_BYTES) {
        data_bytes = RECORDING_MAX_DATA_BYTES;
        if (truncate(part, (off_t)(WAV_HEADER_BYTES + data_bytes)) != 0) return;
    }
    data_bytes &= ~1ULL;
    if (truncate(part, (off_t)(WAV_HEADER_BYTES + data_bytes)) != 0) return;
    fd = open(part, O_RDWR);
    if (fd < 0) return;
    int repair_error = write_wav_header(fd, data_bytes);
    if (!repair_error && fsync(fd) < 0) repair_error = -errno;
    if (close(fd) < 0) repair_error = -errno;
    if (repair_error) return;
    snprintf(final, sizeof(final), "%s/%.*s", RECORDINGS_ROOT,
             (int)(length - 5), name);
    if (access(final, F_OK) == 0) g_recording.last_error = -EEXIST;
    else if (rename(part, final) != 0) g_recording.last_error = -errno;
}

static int save_end_metadata(const char *id, const char *reason, int error)
{
    char path[RECORDING_PATH_MAX], tmp[RECORDING_PATH_MAX], json[160];
    int fd, ret;
    snprintf(path, sizeof(path), "%s/%s.json", RECORDINGS_ROOT, id);
    snprintf(tmp, sizeof(tmp), "%s/%s.json.tmp", RECORDINGS_ROOT, id);
    snprintf(json, sizeof(json), "{\"end_reason\":\"%s\",\"error\":%d}", reason, error);
    fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return -errno;
    ret = write_all(fd, json, strlen(json));
    if (!ret && fsync(fd) < 0) ret = -errno;
    if (close(fd) < 0) ret = -errno;
    if (!ret && rename(tmp, path) < 0) ret = -errno;
    return ret;
}

static void recover_part_files(void)
{
    DIR *dir = opendir(RECORDINGS_ROOT);
    struct dirent *entry;
    if (!dir) return;
    while ((entry = readdir(dir)) != NULL)
        repair_part_file(entry->d_name);
    closedir(dir);
}

static void *recording_thread(void *arg)
{
    unsigned char chunk[RECORDING_CHUNK_BYTES];
    unsigned long long synced_at = 0;
    int final_error = 0;
    int publish_error = 0;
    const char *reason = "user_stop";
    (void)arg;

    for (;;) {
        audio_capture_t *capture;
        int fd;
        unsigned long long remaining;
        bool stop;
        int read_bytes;

        pthread_mutex_lock(&g_recording.lock);
        capture = g_recording.capture;
        fd = g_recording.fd;
        stop = g_recording.stop_requested;
        remaining = g_recording.max_data_bytes - g_recording.data_bytes;
        pthread_mutex_unlock(&g_recording.lock);
        if (stop || remaining == 0) {
            if (!stop) reason = g_recording.max_data_bytes == RECORDING_MAX_DATA_BYTES ?
                "duration_limit" : "space_limit";
            break;
        }

        read_bytes = audio_capture_read(capture, chunk, sizeof(chunk));
        if (read_bytes <= 0) {
            pthread_mutex_lock(&g_recording.lock);
            stop = g_recording.stop_requested;
            pthread_mutex_unlock(&g_recording.lock);
            if (stop) break; /* aborting a blocking read is a normal stop */
            if (read_bytes == -EAGAIN || read_bytes == -EINTR) {
                usleep(1000);
                continue;
            }
            final_error = read_bytes ? read_bytes : -EIO;
            reason = "capture_error";
            break;
        }
        if ((unsigned long long)read_bytes > remaining)
            read_bytes = (int)remaining;
        if (write_all(fd, chunk, (size_t)read_bytes) < 0) {
            final_error = -EIO;
            reason = "storage_error";
            break;
        }

        pthread_mutex_lock(&g_recording.lock);
        g_recording.data_bytes += (unsigned long long)read_bytes;
        if (g_recording.data_bytes - synced_at >= RECORDING_SYNC_BYTES) {
            if (fsync(fd) < 0) {
                final_error = -errno;
                reason = "storage_error";
            }
            synced_at = g_recording.data_bytes;
        }
        pthread_mutex_unlock(&g_recording.lock);
        if (final_error) break;
    }

    pthread_mutex_lock(&g_recording.lock);
    g_recording.phase = REC_FINALIZING;
    if (g_recording.fd >= 0) {
        /* Also account for a partial write: keep complete PCM frames only. */
        off_t end = lseek(g_recording.fd, 0, SEEK_END);
        if (end < (off_t)WAV_HEADER_BYTES) publish_error = -EIO;
        else {
            g_recording.data_bytes = ((unsigned long long)end - WAV_HEADER_BYTES) & ~1ULL;
            if (ftruncate(g_recording.fd, (off_t)(WAV_HEADER_BYTES + g_recording.data_bytes)) < 0 ||
                write_wav_header(g_recording.fd, g_recording.data_bytes) < 0 ||
                fsync(g_recording.fd) < 0) publish_error = -EIO;
        }
        if (close(g_recording.fd) < 0) publish_error = -EIO;
        g_recording.fd = -1;
    }
    if (g_recording.capture) {
        audio_capture_close(g_recording.capture);
        g_recording.capture = NULL;
    }
    if (!publish_error && rename(g_recording.part_path, g_recording.final_path) != 0)
        publish_error = -errno;
    g_recording.published = publish_error == 0;
    g_recording.stop_requested = false;
    g_recording.last_error = publish_error ? publish_error : final_error;
    snprintf(g_recording.end_reason, sizeof(g_recording.end_reason), "%s",
             publish_error ? "publish_failed" : reason);
    if (g_recording.published &&
        save_end_metadata(g_recording.id, reason, final_error) < 0)
        g_recording.last_error = -EIO; /* WAV remains playable; metadata warning. */
    pthread_mutex_unlock(&g_recording.lock);

    audio_arbiter_release(AUDIO_OWNER_RECORDING);
    resume_wakeword();
    pthread_mutex_lock(&g_recording.lock);
    g_recording.active = false;
    g_recording.phase = REC_IDLE;
    pthread_cond_broadcast(&g_recording.changed);
    pthread_mutex_unlock(&g_recording.lock);
    syslog(final_error ? LOG_ERR : LOG_INFO,
           "[%s] recording %s finished: bytes=%llu error=%d\n", TAG,
           g_recording.id, g_recording.data_bytes, final_error);
    return NULL;
}

static void collect_finished_thread(void)
{
    pthread_t thread;
    bool joinable = false;

    pthread_mutex_lock(&g_recording.lock);
    if (g_recording.thread_started && !g_recording.active && !g_recording.joining) {
        thread = g_recording.thread;
        joinable = true;
        g_recording.joining = true;
    }
    pthread_mutex_unlock(&g_recording.lock);
    if (!joinable) return;
    pthread_join(thread, NULL);
    pthread_mutex_lock(&g_recording.lock);
    if (!g_recording.active && pthread_equal(g_recording.thread, thread))
        g_recording.thread_started = false;
    g_recording.joining = false;
    pthread_cond_broadcast(&g_recording.changed);
    pthread_mutex_unlock(&g_recording.lock);
}

int recording_service_init(void)
{
    int ret;
    pthread_mutex_lock(&g_recording.lock);
    if (g_recording.initialized) {
        pthread_mutex_unlock(&g_recording.lock);
        return 0;
    }
    ret = mkdir_checked(RECORDINGS_PARENT);
    if (ret == 0) ret = mkdir_checked(RECORDINGS_ROOT);
    if (ret == 0) {
        g_recording.initialized = true;
        recover_part_files();
    }
    pthread_mutex_unlock(&g_recording.lock);
    return ret;
}

int recording_service_start(char *id, size_t id_size)
{
    audio_capture_t *capture;
    pthread_attr_t attr;
    unsigned long long allowed;
    unsigned int number;
    int fd;
    int ret;

    if (!id || id_size < RECORDING_ID_MAX) return -EINVAL;
    ret = recording_service_init();
    if (ret < 0) return ret;

    collect_finished_thread();

    pthread_mutex_lock(&g_recording.lock);
    if (g_recording.active || g_recording.thread_started) {
        pthread_mutex_unlock(&g_recording.lock);
        return -EBUSY;
    }
    allowed = permitted_data_bytes();
    if (allowed < RECORDING_BYTES_PER_SECOND * RECORDING_MIN_SECONDS) {
        pthread_mutex_unlock(&g_recording.lock);
        return -ENOSPC;
    }
    number = next_recording_number();
    snprintf(g_recording.id, sizeof(g_recording.id), "rec-%06u", number);
    snprintf(g_recording.part_path, sizeof(g_recording.part_path), "%s/%s.wav.part",
             RECORDINGS_ROOT, g_recording.id);
    snprintf(g_recording.final_path, sizeof(g_recording.final_path), "%s/%s.wav",
             RECORDINGS_ROOT, g_recording.id);
    g_recording.max_data_bytes = allowed;
    g_recording.data_bytes = 0;
    g_recording.last_error = 0;
    g_recording.published = false;
    g_recording.end_reason[0] = 0;
    g_recording.phase = REC_STARTING;
    g_recording.active = true; /* Reserve before releasing the mutex. */
    pthread_mutex_unlock(&g_recording.lock);

    ret = acquire_recording_audio();
    if (ret < 0) goto start_failed;
    capture = audio_capture_open_with_gain(AGENT_AUDIO_CAPTURE_DEV,
        RECORDING_RATE, RECORDING_CHANNELS, RECORDING_BITS, 1);
    if (!capture || audio_capture_start(capture) < 0) {
        if (capture) audio_capture_close(capture);
        audio_arbiter_release(AUDIO_OWNER_RECORDING);
        resume_wakeword();
        ret = -EIO; goto start_failed;
    }
    fd = open(g_recording.part_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0 || write_wav_header(fd, 0) < 0) {
        if (fd >= 0) close(fd);
        unlink(g_recording.part_path);
        audio_capture_close(capture);
        audio_arbiter_release(AUDIO_OWNER_RECORDING);
        resume_wakeword();
        ret = -EIO; goto start_failed;
    }

    pthread_mutex_lock(&g_recording.lock);
    g_recording.capture = capture;
    g_recording.fd = fd;
    g_recording.active = true;
    g_recording.thread_started = true;
    g_recording.phase = REC_RECORDING;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8192);
    ret = pthread_create(&g_recording.thread, &attr, recording_thread, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        g_recording.stop_requested = true;
        g_recording.thread_started = false;
        g_recording.phase = REC_FINALIZING;
    }
    if (!ret) snprintf(id, id_size, "%s", g_recording.id);
    pthread_mutex_unlock(&g_recording.lock);
    if (ret != 0) {
        audio_capture_abort(capture);
        recording_thread(NULL);
        return -ret;
    }
    return 0;
start_failed:
    pthread_mutex_lock(&g_recording.lock);
    g_recording.active = false;
    g_recording.phase = REC_IDLE;
    g_recording.last_error = ret;
    snprintf(g_recording.end_reason, sizeof(g_recording.end_reason), "start_failed");
    pthread_cond_broadcast(&g_recording.changed);
    pthread_mutex_unlock(&g_recording.lock);
    return ret;
}

int recording_service_stop(const char *id)
{
    pthread_t thread;
    audio_capture_t *capture;
    if (!valid_id(id)) return -EINVAL;
    pthread_mutex_lock(&g_recording.lock);
    if (strcmp(id, g_recording.id) != 0) {
        pthread_mutex_unlock(&g_recording.lock);
        return -ENOENT;
    }
    if (g_recording.phase == REC_STARTING ||
        (g_recording.active && !g_recording.thread_started)) {
        pthread_mutex_unlock(&g_recording.lock);
        return -EBUSY;
    }
    if (!g_recording.active) {
        int result = g_recording.published ? 0 : -EIO;
        pthread_mutex_unlock(&g_recording.lock);
        return result;
    }
    g_recording.stop_requested = true;
    if (g_recording.phase == REC_RECORDING) g_recording.phase = REC_STOPPING;
    capture = g_recording.capture;
    thread = g_recording.thread;
    /* Protect the capture lifetime until abort has returned. */
    if (capture) audio_capture_abort(capture);
    if (g_recording.joining) {
        while (g_recording.active) pthread_cond_wait(&g_recording.changed, &g_recording.lock);
        int result = g_recording.published ? 0 : -EIO;
        pthread_mutex_unlock(&g_recording.lock);
        return result;
    }
    g_recording.joining = true;
    pthread_mutex_unlock(&g_recording.lock);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&g_recording.lock);
    g_recording.thread_started = false;
    g_recording.joining = false;
    int result = g_recording.published ? 0 : -EIO;
    pthread_cond_broadcast(&g_recording.changed);
    pthread_mutex_unlock(&g_recording.lock);
    return result;
}

int recording_service_delete(const char *id)
{
    char path[RECORDING_PATH_MAX];
    if (!valid_id(id)) return -EINVAL;
    pthread_mutex_lock(&g_recording.lock);
    if (g_recording.active && strcmp(id, g_recording.id) == 0) {
        pthread_mutex_unlock(&g_recording.lock);
        return -EBUSY;
    }
    pthread_mutex_unlock(&g_recording.lock);
    snprintf(path, sizeof(path), "%s/%s.wav", RECORDINGS_ROOT, id);
    if (unlink(path) < 0) return -errno;
    snprintf(path, sizeof(path), "%s/%s.json", RECORDINGS_ROOT, id);
    if (unlink(path) < 0 && errno != ENOENT) return -errno;
    return 0;
}

int recording_service_resolve_audio(const char *id, char *path,
                                    size_t path_size,
                                    unsigned long long *file_size)
{
    struct stat st;
    if (!valid_id(id) || !path || !file_size) return -EINVAL;
    snprintf(path, path_size, "%s/%s.wav", RECORDINGS_ROOT, id);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -ENOENT;
    *file_size = (unsigned long long)st.st_size;
    return 0;
}

struct cJSON *recording_service_snapshot_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "recordings");
    DIR *dir;
    struct dirent *entry;
    unsigned long long used = recording_bytes_used();
    unsigned long long free_bytes = recording_free_bytes();
    unsigned long long allowed = permitted_data_bytes();

    pthread_mutex_lock(&g_recording.lock);
    cJSON_AddBoolToObject(root, "active", g_recording.active);
    static const char *const phases[] = {"IDLE", "STARTING", "RECORDING", "STOPPING", "FINALIZING"};
    cJSON_AddStringToObject(root, "state", phases[g_recording.phase]);
    cJSON_AddStringToObject(root, "end_reason", g_recording.end_reason);
    cJSON_AddBoolToObject(root, "published", g_recording.published);
    if (g_recording.active) {
        cJSON *active = cJSON_AddObjectToObject(root, "current");
        cJSON_AddStringToObject(active, "id", g_recording.id);
        cJSON_AddNumberToObject(active, "data_bytes", (double)g_recording.data_bytes);
        cJSON_AddNumberToObject(active, "duration_seconds",
                                (double)(g_recording.data_bytes / RECORDING_BYTES_PER_SECOND));
        cJSON_AddNumberToObject(active, "max_duration_seconds",
                                (double)(g_recording.max_data_bytes / RECORDING_BYTES_PER_SECOND));
    }
    cJSON_AddNumberToObject(root, "last_error", g_recording.last_error);
    pthread_mutex_unlock(&g_recording.lock);
    cJSON_AddNumberToObject(root, "quota_bytes", (double)RECORDING_QUOTA_BYTES);
    cJSON_AddNumberToObject(root, "used_bytes", (double)used);
    cJSON_AddNumberToObject(root, "free_bytes", (double)free_bytes);
    cJSON_AddNumberToObject(root, "available_duration_seconds",
                            (double)(allowed / RECORDING_BYTES_PER_SECOND));
    cJSON_AddNumberToObject(root, "max_duration_seconds", RECORDING_MAX_SECONDS);

    dir = opendir(RECORDINGS_ROOT);
    if (!dir) return root;
    while ((entry = readdir(dir)) != NULL) {
        char path[RECORDING_PATH_MAX];
        char id[RECORDING_ID_MAX];
        struct stat st;
        cJSON *item;
        if (!ends_with(entry->d_name, ".wav")) continue;
        snprintf(id, sizeof(id), "%.*s", (int)(strlen(entry->d_name) - 4), entry->d_name);
        if (!valid_id(id)) continue;
        snprintf(path, sizeof(path), "%s/%s", RECORDINGS_ROOT, entry->d_name);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < WAV_HEADER_BYTES)
            continue;
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddNumberToObject(item, "size_bytes", (double)st.st_size);
        cJSON_AddNumberToObject(item, "duration_seconds",
                                (double)((st.st_size - WAV_HEADER_BYTES) / RECORDING_BYTES_PER_SECOND));
        cJSON_AddNumberToObject(item, "modified_epoch", (double)st.st_mtime);
        snprintf(path, sizeof(path), "%s/%s.json", RECORDINGS_ROOT, id);
        FILE *meta = fopen(path, "r");
        if (meta) {
            char json[256];
            size_t size = fread(json, 1, sizeof(json) - 1, meta);
            fclose(meta); json[size] = 0;
            cJSON *saved = cJSON_Parse(json);
            cJSON *reason = saved ? cJSON_GetObjectItem(saved, "end_reason") : NULL;
            cJSON *error = saved ? cJSON_GetObjectItem(saved, "error") : NULL;
            if (cJSON_IsString(reason)) cJSON_AddStringToObject(item, "end_reason", reason->valuestring);
            if (cJSON_IsNumber(error)) cJSON_AddNumberToObject(item, "error", error->valuedouble);
            cJSON_Delete(saved);
        }
        cJSON_AddItemToArray(items, item);
    }
    closedir(dir);
    return root;
}
