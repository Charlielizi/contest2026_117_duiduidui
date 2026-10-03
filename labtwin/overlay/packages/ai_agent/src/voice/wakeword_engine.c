#include "voice/wakeword_engine.h"
#include "voice/audio_arbiter.h"
#include "voice/audio_capture.h"
#ifdef WAKEWORD_HOST_TEST
#define AGENT_VOICE_SAMPLE_RATE 16000
#define AGENT_VOICE_BITS 16
#define AGENT_AUDIO_CAPTURE_DEV "/dev/null"
#define AGENT_WAKEWORD_INFER_INTERVAL_MS 100
#define AGENT_WAKEWORD_THRESHOLD 0.85f
#define AGENT_WAKEWORD_CONSECUTIVE 3
#else
#include "agent_config.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define TAG "wakeword"
#define WAKE_FRAME_SAMPLES \
    (AGENT_VOICE_SAMPLE_RATE * AGENT_WAKEWORD_INFER_INTERVAL_MS / 1000)
#define WAKE_THREAD_STACK_SIZE (32 * 1024)
#define WAKE_THREAD_PRIORITY 50
#define WAKE_CAPTURE_RETRY_MIN_MS 1000
#define WAKE_CAPTURE_RETRY_MAX_MS 10000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const wakeword_engine_ops_t *g_ops;
static wakeword_detected_cb_t g_callback;
static void *g_callback_arg;
static pthread_t g_thread;
static int g_running;
static int g_paused = 1;
static int g_consecutive;
static wakeword_language_t g_last_language;
static wakeword_stats_t g_stats;
/* Keep the 100 ms capture buffer off the pthread stack.  The default NuttX
 * pthread stack is only 4 KiB, while this buffer alone is 3.2 KiB at 16 kHz.
 */
static int16_t g_frame[WAKE_FRAME_SAMPLES];

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t elapsed_us(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint32_t)((end->tv_sec - start->tv_sec) * 1000000ULL +
        (end->tv_nsec - start->tv_nsec) / 1000);
}

static void publish_detection(wakeword_language_t language, float score)
{
    wakeword_detected_cb_t callback;
    void *arg;
    wakeword_detection_t detection;

    pthread_mutex_lock(&g_lock);
    g_paused = 1;
    g_consecutive = 0;
    g_stats.detections++;
    callback = g_callback;
    arg = g_callback_arg;
    detection.language = language;
    detection.score = score;
    detection.monotonic_ms = mono_ms();
    detection.model_version = g_ops ? g_ops->model_version : "unknown";
    pthread_mutex_unlock(&g_lock);

    if (callback) callback(&detection, arg);
}

static void *wake_thread(void *arg)
{
    unsigned int capture_retry_ms = WAKE_CAPTURE_RETRY_MIN_MS;

    (void)arg;

    while (g_running) {
        audio_capture_t *capture = NULL;

        pthread_mutex_lock(&g_lock);
        int paused = g_paused;
        pthread_mutex_unlock(&g_lock);
        if (paused || !g_ops) {
            usleep(20 * 1000);
            continue;
        }

        if (audio_arbiter_acquire(AUDIO_OWNER_WAKEWORD) != 0) {
            usleep(20 * 1000);
            continue;
        }

        capture = audio_capture_open(AGENT_AUDIO_CAPTURE_DEV,
            AGENT_VOICE_SAMPLE_RATE, 1, AGENT_VOICE_BITS);
        if (!capture || audio_capture_start(capture) < 0) {
            if (capture) audio_capture_close(capture);
            audio_arbiter_release(AUDIO_OWNER_WAKEWORD);
            pthread_mutex_lock(&g_lock);
            g_stats.capture_errors++;
            pthread_mutex_unlock(&g_lock);
            usleep(capture_retry_ms * 1000);
            if (capture_retry_ms < WAKE_CAPTURE_RETRY_MAX_MS / 2)
                capture_retry_ms *= 2;
            else
                capture_retry_ms = WAKE_CAPTURE_RETRY_MAX_MS;
            continue;
        }

        int capture_failed = 0;
        while (g_running) {
            pthread_mutex_lock(&g_lock);
            paused = g_paused;
            pthread_mutex_unlock(&g_lock);
            if (paused) break;

            int n = audio_capture_read(capture, g_frame, sizeof(g_frame));
            if (n <= 0) {
                if (n == -EAGAIN || n == -EWOULDBLOCK) continue;
                pthread_mutex_lock(&g_lock);
                g_stats.capture_errors++;
                pthread_mutex_unlock(&g_lock);
                capture_failed = 1;
                break;
            }

            capture_retry_ms = WAKE_CAPTURE_RETRY_MIN_MS;

            wakeword_language_t language = WAKEWORD_LANG_ZH;
            float score = 0.0f;
            struct timespec start, end;
            clock_gettime(CLOCK_MONOTONIC, &start);
            int ret = g_ops->process(g_frame, (size_t)n / sizeof(int16_t),
                                     &language, &score);
            clock_gettime(CLOCK_MONOTONIC, &end);

            pthread_mutex_lock(&g_lock);
            g_stats.frames++;
            g_stats.inferences++;
            g_stats.last_inference_us = elapsed_us(&start, &end);
            if (g_stats.last_inference_us > g_stats.max_inference_us)
                g_stats.max_inference_us = g_stats.last_inference_us;

            if (ret > 0 && score >= AGENT_WAKEWORD_THRESHOLD) {
                if (g_consecutive == 0 || language == g_last_language)
                    g_consecutive++;
                else
                    g_consecutive = 1;
                g_last_language = language;
            } else {
                g_consecutive = 0;
            }
            int detected = g_consecutive >= AGENT_WAKEWORD_CONSECUTIVE;
            pthread_mutex_unlock(&g_lock);

            if (detected) {
                audio_capture_abort(capture);
                audio_capture_close(capture);
                capture = NULL;
                audio_arbiter_release(AUDIO_OWNER_WAKEWORD);
                publish_detection(language, score);
                break;
            }
        }

        if (capture) {
            audio_capture_abort(capture);
            audio_capture_close(capture);
            audio_arbiter_release(AUDIO_OWNER_WAKEWORD);
            if (capture_failed) {
                usleep(capture_retry_ms * 1000);
                if (capture_retry_ms < WAKE_CAPTURE_RETRY_MAX_MS / 2)
                    capture_retry_ms *= 2;
                else
                    capture_retry_ms = WAKE_CAPTURE_RETRY_MAX_MS;
            }
        }
    }
    return NULL;
}

int wakeword_engine_register(const wakeword_engine_ops_t *ops)
{
    if (!ops || !ops->name || !ops->process) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    if (g_ops) {
        pthread_mutex_unlock(&g_lock);
        return -EEXIST;
    }
    g_ops = ops;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int wakeword_engine_init(wakeword_detected_cb_t callback, void *arg)
{
    pthread_mutex_lock(&g_lock);
    g_callback = callback;
    g_callback_arg = arg;
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_lock);
    if (!g_ops) return -ENODEV;
    return g_ops->init ? g_ops->init() : 0;
}

int wakeword_engine_start(void)
{
    pthread_attr_t attr;
    int attr_ready = 0;
    int ret;

    pthread_mutex_lock(&g_lock);
    if (g_running) {
        g_paused = 0;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_running = 1;
    g_paused = 0;
    pthread_mutex_unlock(&g_lock);
    ret = pthread_attr_init(&attr);
    if (ret == 0) attr_ready = 1;
    if (ret == 0) ret = pthread_attr_setstacksize(&attr,
                                                   WAKE_THREAD_STACK_SIZE);
#ifndef WAKEWORD_HOST_TEST
    if (ret == 0) {
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = WAKE_THREAD_PRIORITY;
        ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        if (ret == 0)
            ret = pthread_attr_setschedparam(&attr, &param);
        if (ret == 0)
            ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }
#endif
    if (ret == 0) ret = pthread_create(&g_thread, &attr, wake_thread, NULL);
    if (attr_ready) pthread_attr_destroy(&attr);
    if (ret != 0) {
        pthread_mutex_lock(&g_lock);
        g_running = 0;
        g_paused = 1;
        pthread_mutex_unlock(&g_lock);
        return -ret;
    }
    return 0;
}

int wakeword_engine_pause(void)
{
    pthread_mutex_lock(&g_lock);
    g_paused = 1;
    g_consecutive = 0;
    if (g_ops && g_ops->reset) g_ops->reset();
    pthread_mutex_unlock(&g_lock);
    return 0;
}

bool wakeword_engine_is_listening(void)
{
    bool listening;
    pthread_mutex_lock(&g_lock);
    listening = g_running && !g_paused;
    pthread_mutex_unlock(&g_lock);
    return listening;
}

int wakeword_engine_resume(void)
{
    pthread_mutex_lock(&g_lock);
    g_consecutive = 0;
    if (g_ops && g_ops->reset) g_ops->reset();
    g_paused = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void wakeword_engine_stop(void)
{
    pthread_mutex_lock(&g_lock);
    int running = g_running;
    g_running = 0;
    g_paused = 1;
    pthread_mutex_unlock(&g_lock);
    if (running) pthread_join(g_thread, NULL);
    if (g_ops && g_ops->deinit) g_ops->deinit();
}

void wakeword_engine_get_stats(wakeword_stats_t *stats)
{
    if (!stats) return;
    pthread_mutex_lock(&g_lock);
    *stats = g_stats;
    pthread_mutex_unlock(&g_lock);
}

const char *wakeword_engine_backend(void)
{
    return g_ops ? g_ops->name : "none";
}

int wakeword_engine_test_detection(wakeword_language_t language, float score)
{
    if (score < AGENT_WAKEWORD_THRESHOLD) return -ERANGE;
    publish_detection(language, score);
    return 0;
}
