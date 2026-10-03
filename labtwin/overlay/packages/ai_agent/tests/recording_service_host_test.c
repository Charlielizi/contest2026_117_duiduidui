#include "voice/recording_service.h"
#include "voice/audio_arbiter.h"
#include "voice/audio_capture.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

struct audio_capture { int placeholder; };
static struct audio_capture g_capture;
static atomic_bool aborted;
static atomic_int reads, closes;
static int mode;
enum { NORMAL, INTERRUPTED, CAPTURE_ERROR, EOF_MODE, RETRY_MODE, SLOW_OPEN };

audio_capture_t *audio_capture_open_with_gain(const char *dev, unsigned int rate,
    unsigned int channels, unsigned int bits, unsigned int gain)
{
    (void)dev;
    assert(rate == 16000 && channels == 1 && bits == 16 && gain == 1);
    if (mode == SLOW_OPEN) usleep(100000);
    atomic_store(&aborted, false);
    atomic_store(&reads, 0);
    return &g_capture;
}
int audio_capture_start(audio_capture_t *capture) { return capture ? 0 : -1; }
int audio_capture_read(audio_capture_t *capture, void *buffer, size_t length)
{
    (void)capture;
    int call = atomic_fetch_add(&reads, 1);
    if (mode == INTERRUPTED && call > 0) {
        while (!atomic_load(&aborted)) usleep(1000);
        return -ECANCELED;
    }
    if (mode == CAPTURE_ERROR && call > 0) return -EIO;
    if (mode == EOF_MODE && call > 0) return 0;
    if (mode == RETRY_MODE && call == 0) return -EAGAIN;
    usleep(1000);
    memset(buffer, 0x11, length);
    return (int)length;
}
int audio_capture_abort(audio_capture_t *capture) { (void)capture; atomic_store(&aborted, true); return 0; }
void audio_capture_close(audio_capture_t *capture) { (void)capture; atomic_fetch_add(&closes, 1); }
int audio_arbiter_acquire(audio_owner_t owner) { return owner ? 0 : -1; }
void audio_arbiter_release(audio_owner_t owner) { (void)owner; }
audio_owner_t audio_arbiter_owner(void) { return AUDIO_OWNER_NONE; }
const char *audio_arbiter_owner_name(audio_owner_t owner) { (void)owner; return "test"; }

static char concurrent_id[16];
static int concurrent_start_result;
static void *start_worker(void *unused)
{
    (void)unused;
    concurrent_start_result = recording_service_start(concurrent_id, sizeof(concurrent_id));
    return NULL;
}
static void *stop_worker(void *id)
{
    assert(recording_service_stop(id) == 0);
    return NULL;
}

int main(void)
{
    char id[16];
    char path[128];
    unsigned long long size = 0;
    unsigned char header[4];
    int fd;
    cJSON *snapshot;

    mkdir("/tmp/labtwin-recordings-host", 0700);
    mkdir("/tmp/labtwin-recordings-host/recordings", 0700);
    fd = open("/tmp/labtwin-recordings-host/recordings/rec-000001.wav.part",
              O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, "00000000000000000000000000000000000000000000", 44) == 44);
    assert(write(fd, "test", 4) == 4);
    close(fd);

    assert(recording_service_init() == 0);
    assert(recording_service_resolve_audio("rec-000001", path, sizeof(path), &size) == 0);
    assert(size == 48);
    assert(recording_service_start(id, sizeof(id)) == 0);
    usleep(20000);
    assert(recording_service_stop(id) == 0);
    assert(recording_service_resolve_audio(id, path, sizeof(path), &size) == 0);
    fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, header, sizeof(header)) == 4);
    close(fd);
    assert(memcmp(header, "RIFF", 4) == 0);
    snapshot = recording_service_snapshot_json();
    assert(snapshot && cJSON_GetArraySize(cJSON_GetObjectItem(snapshot, "recordings")) >= 2);
    cJSON_Delete(snapshot);
    assert(recording_service_delete(id) == 0);
    mode = INTERRUPTED;
    assert(recording_service_start(id, sizeof(id)) == 0);
    while (atomic_load(&reads) < 2) usleep(1000);
    int previous_closes = atomic_load(&closes);
    pthread_t a, b;
    assert(pthread_create(&a, NULL, stop_worker, id) == 0);
    assert(pthread_create(&b, NULL, stop_worker, id) == 0);
    pthread_join(a, NULL); pthread_join(b, NULL);
    assert(atomic_load(&closes) == previous_closes + 1);
    assert(recording_service_stop(id) == 0);
    assert(recording_service_resolve_audio(id, path, sizeof(path), &size) == 0 && size > 44);
    snapshot = recording_service_snapshot_json();
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(snapshot, "end_reason")), "user_stop") == 0);
    cJSON_Delete(snapshot);
    assert(recording_service_delete(id) == 0);
    for (mode = CAPTURE_ERROR; mode <= RETRY_MODE; mode++) {
        assert(recording_service_start(id, sizeof(id)) == 0);
        usleep(20000);
        assert(recording_service_stop(id) == 0);
        assert(recording_service_resolve_audio(id, path, sizeof(path), &size) == 0 && size > 44);
        assert(recording_service_delete(id) == 0);
    }
    mode = SLOW_OPEN;
    assert(pthread_create(&a, NULL, start_worker, NULL) == 0);
    usleep(20000);
    assert(recording_service_start(id, sizeof(id)) == -EBUSY);
    pthread_join(a, NULL);
    assert(concurrent_start_result == 0);
    assert(recording_service_stop(concurrent_id) == 0);
    assert(recording_service_delete(concurrent_id) == 0);
    mode = NORMAL;
    assert(recording_service_start(id, sizeof(id)) == 0);
    snprintf(path, sizeof(path), "/tmp/labtwin-recordings-host/recordings/%s.wav", id);
    assert(mkdir(path, 0700) == 0); /* deterministic publication failure */
    usleep(20000);
    assert(recording_service_stop(id) != 0);
    assert(rmdir(path) == 0);
    puts("recording_service_host_test: PASS");
    return 0;
}
