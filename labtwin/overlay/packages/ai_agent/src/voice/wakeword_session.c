#include "voice/wakeword_session.h"
#include "voice/audio_arbiter.h"
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"
#include "voice/voice_cloud_guard.h"
#include "agent_config.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int wakeword_tflm_register(void);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static wake_session_snapshot_t g_snapshot;
static uint32_t g_request_seq;
static int g_pending_during_tts;
static int g_wake_available;
static int g_new_experiment_mode;

/* Tone generation alone uses 3840 bytes; never use the RTOS default stack. */
static int start_session_worker(pthread_t *thread, void *(*entry)(void *))
{
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_attr_setstacksize(&attr, 24 * 1024);
    if (rc == 0) rc = pthread_create(thread, &attr, entry, NULL);
    pthread_attr_destroy(&attr);
    return rc;
}

static void set_state(wake_session_state_t state, const char *error)
{
    pthread_mutex_lock(&g_lock);
    g_snapshot.state = state;
    snprintf(g_snapshot.error, sizeof(g_snapshot.error), "%s",
             error ? error : "");
    pthread_mutex_unlock(&g_lock);
}

static void restore_ready_state(const char *error)
{
    int disabled;

    pthread_mutex_lock(&g_lock);
    disabled = g_snapshot.state == WAKE_SESSION_DISABLED;
    pthread_mutex_unlock(&g_lock);
    if (disabled) {
        voice_cloud_guard_end();
        return;
    }
    if (g_wake_available) wakeword_engine_resume();
    voice_cloud_guard_end();
    set_state(g_wake_available ? WAKE_SESSION_LOCAL_LISTENING :
                                WAKE_SESSION_PTT_READY, error);
}

static void finish_with_cooldown(const char *error)
{
    set_state(WAKE_SESSION_COOLDOWN, error);
    usleep(AGENT_WAKEWORD_COOLDOWN_MS * 1000);
    restore_ready_state(error);
}

static void *cooldown_worker(void *arg)
{
    (void)arg;
    usleep(AGENT_WAKEWORD_COOLDOWN_MS * 1000);
    restore_ready_state(NULL);
    return NULL;
}

static void finish_with_cooldown_async(void)
{
    pthread_t thread;

    set_state(WAKE_SESSION_COOLDOWN, NULL);
    if (start_session_worker(&thread, cooldown_worker) == 0) {
        pthread_detach(thread);
    } else {
        restore_ready_state(NULL);
    }
}

static int command_session_active(void)
{
    int active;

    pthread_mutex_lock(&g_lock);
    active = g_snapshot.state == WAKE_SESSION_WAKE_DETECTED ||
             g_snapshot.state == WAKE_SESSION_COMMAND_RECORDING;
    pthread_mutex_unlock(&g_lock);
    return active;
}

static void play_wake_tone(void)
{
    enum { RATE = 24000, SAMPLES = 1920 };
    int16_t pcm[SAMPLES];
    if (audio_arbiter_acquire(AUDIO_OWNER_TTS) != 0) return;
    for (int i = 0; i < SAMPLES; i++) {
        double envelope = i < 240 ? (double)i / 240.0 :
            (i > SAMPLES - 240 ? (double)(SAMPLES - i) / 240.0 : 1.0);
        pcm[i] = (int16_t)(sin(2.0 * 3.141592653589793 * 880.0 * i / RATE) *
                           6000.0 * envelope);
    }
    audio_playback_t *pb = audio_playback_open(
        AGENT_AUDIO_PLAYBACK_DEV, RATE, 1, 16);
    if (pb) {
        audio_playback_write(pb, pcm, sizeof(pcm));
        audio_playback_close(pb);
    }
    audio_arbiter_release(AUDIO_OWNER_TTS);
}

static void *command_session_thread(void *arg)
{
    (void)arg;
    if (voice_channel_is_ready() != 0) {
        finish_with_cooldown("ASR credentials not configured");
        return NULL;
    }
    play_wake_tone();
    if (!command_session_active()) return NULL;
    set_state(WAKE_SESSION_COMMAND_RECORDING, NULL);
    voice_cloud_guard_set_phase(CLOUD_PHASE_RECORDING);
    if (voice_channel_start_auto(8000, 800) != 0) {
        finish_with_cooldown("microphone busy");
        return NULL;
    }

    while (!voice_channel_auto_done()) {
        if (!command_session_active()) return NULL;
        usleep(20 * 1000);
    }
    if (!command_session_active()) return NULL;
    set_state(WAKE_SESSION_ASR, NULL);
    if (voice_channel_stop() != 0) {
        finish_with_cooldown("ASR failed");
    }
    return NULL;
}

static void on_detected(const wakeword_detection_t *detection, void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_lock);
    g_snapshot.state = WAKE_SESSION_WAKE_DETECTED;
    g_snapshot.language = detection->language;
    g_snapshot.score = detection->score;
    snprintf(g_snapshot.model_version, sizeof(g_snapshot.model_version), "%s",
             detection->model_version ? detection->model_version : "unknown");
    snprintf(g_snapshot.request_id, sizeof(g_snapshot.request_id),
             "wake-%llu-%lu", (unsigned long long)detection->monotonic_ms,
             (unsigned long)++g_request_seq);
    g_snapshot.transcript[0] = 0;
    g_snapshot.reply[0] = 0;
    g_snapshot.error[0] = 0;
    pthread_mutex_unlock(&g_lock);
    voice_cloud_guard_begin("wake", g_snapshot.request_id);

    pthread_t thread;
    if (start_session_worker(&thread, command_session_thread) == 0)
        pthread_detach(thread);
    else {
        wakeword_engine_resume();
        set_state(WAKE_SESSION_LOCAL_LISTENING, "session thread failed");
    }
}

int wakeword_session_init(void)
{
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_wake_available = 0;
    g_snapshot.state = WAKE_SESSION_DISABLED;

    /* A wake detector is useful only when a detected command can reach an
     * ASR service. Do not keep the DMIC and inference thread running when
     * speech credentials are absent: it wastes CPU and causes continuous
     * capture overruns while the UI already reports voice as unavailable. */
    if (voice_channel_is_ready() != 0) {
        set_state(WAKE_SESSION_PTT_READY,
                  "ASR credentials not configured; wake listening paused");
        return 0;
    }

    int ret = wakeword_tflm_register();
    if (ret != 0 && ret != -EEXIST) {
        set_state(WAKE_SESSION_PTT_READY,
                  "wake model unavailable; tap microphone");
        return 0;
    }
    ret = wakeword_engine_init(on_detected, NULL);
    if (ret != 0) {
        set_state(WAKE_SESSION_PTT_READY,
                  "wake model unavailable; tap microphone");
        return 0;
    }
    ret = wakeword_engine_start();
    if (ret != 0) {
        set_state(WAKE_SESSION_PTT_READY,
                  "wake capture unavailable; tap microphone");
        return 0;
    }
    g_wake_available = 1;
    set_state(WAKE_SESSION_LOCAL_LISTENING, NULL);
    return 0;
}

void wakeword_session_shutdown(void)
{
    wakeword_session_cancel();
    if (g_wake_available) wakeword_engine_stop();
    g_wake_available = 0;
    g_new_experiment_mode = 0;
    voice_cloud_guard_end();
    set_state(WAKE_SESSION_DISABLED, NULL);
}

static int start_ptt(bool new_experiment)
{
    pthread_t thread;
    char request_id[sizeof(g_snapshot.request_id)];

    if (voice_channel_is_ready() != 0) {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_lock);
    if (g_snapshot.state != WAKE_SESSION_PTT_READY &&
        g_snapshot.state != WAKE_SESSION_LOCAL_LISTENING) {
        pthread_mutex_unlock(&g_lock);
        return -EBUSY;
    }
    g_snapshot.state = WAKE_SESSION_COMMAND_RECORDING;
    snprintf(g_snapshot.request_id, sizeof(g_snapshot.request_id),
             "ptt-%llu-%lu", (unsigned long long)time(NULL),
             (unsigned long)++g_request_seq);
    g_snapshot.transcript[0] = 0;
    g_snapshot.reply[0] = 0;
    g_snapshot.error[0] = 0;
    g_new_experiment_mode = new_experiment;
    snprintf(request_id, sizeof(request_id), "%s", g_snapshot.request_id);
    pthread_mutex_unlock(&g_lock);

    if (g_wake_available) wakeword_engine_pause();
    voice_cloud_guard_begin("ptt", request_id);
    if (start_session_worker(&thread, command_session_thread) != 0) {
        restore_ready_state("session thread failed");
        return -EIO;
    }
    pthread_detach(thread);
    return 0;
}

int wakeword_session_start_ptt(void)
{
    return start_ptt(false);
}

int wakeword_session_start_new_experiment_ptt(void)
{
    return start_ptt(true);
}

static void *cancel_session_worker(void *arg)
{
    (void)arg;
    voice_channel_cancel();
    usleep(AGENT_WAKEWORD_COOLDOWN_MS * 1000);
    restore_ready_state(NULL);
    return NULL;
}

int wakeword_session_cancel(void)
{
    pthread_t thread;
    int active;

    pthread_mutex_lock(&g_lock);
    active = g_snapshot.state != WAKE_SESSION_DISABLED &&
             g_snapshot.state != WAKE_SESSION_PTT_READY &&
             g_snapshot.state != WAKE_SESSION_LOCAL_LISTENING;
    if (active) {
        g_snapshot.state = WAKE_SESSION_COOLDOWN;
        g_snapshot.error[0] = '\0';
    }
    g_new_experiment_mode = 0;
    pthread_mutex_unlock(&g_lock);

    if (!active) return 0;
    voice_cloud_guard_end();
    if (start_session_worker(&thread, cancel_session_worker) == 0) {
        pthread_detach(thread);
        return 0;
    }

    voice_channel_cancel();
    restore_ready_state(NULL);
    return -EIO;
}

void wakeword_session_on_asr(const char *text)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_snapshot.transcript, sizeof(g_snapshot.transcript), "%s",
             text ? text : "");
    g_snapshot.state = WAKE_SESSION_LLM;
    pthread_mutex_unlock(&g_lock);
    voice_cloud_guard_set_phase(CLOUD_PHASE_LLM);
}

void wakeword_session_on_partial(const char *text)
{
    pthread_mutex_lock(&g_lock);
    if (g_snapshot.state == WAKE_SESSION_COMMAND_RECORDING ||
        g_snapshot.state == WAKE_SESSION_ASR) {
        snprintf(g_snapshot.transcript, sizeof(g_snapshot.transcript),
                 "%s", text ? text : "");
    }
    pthread_mutex_unlock(&g_lock);
}

void wakeword_session_on_reply(const char *text)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_snapshot.reply, sizeof(g_snapshot.reply), "%s", text ? text : "");
    pthread_mutex_unlock(&g_lock);
}

void wakeword_session_on_ptt_begin(void)
{
    int new_ptt = 0;
    pthread_mutex_lock(&g_lock);
    if (g_snapshot.state == WAKE_SESSION_DISABLED ||
        g_snapshot.state == WAKE_SESSION_PTT_READY ||
        g_snapshot.state == WAKE_SESSION_LOCAL_LISTENING) {
        g_snapshot.state = WAKE_SESSION_COMMAND_RECORDING;
        snprintf(g_snapshot.request_id, sizeof(g_snapshot.request_id),
                 "ptt-%llu-%lu", (unsigned long long)time(NULL),
                 (unsigned long)++g_request_seq);
        g_snapshot.transcript[0] = 0;
        g_snapshot.reply[0] = 0;
        g_snapshot.error[0] = 0;
        if (g_wake_available) wakeword_engine_pause();
        new_ptt = 1;
    }
    char request_id[sizeof(g_snapshot.request_id)];
    snprintf(request_id, sizeof(request_id), "%s", g_snapshot.request_id);
    pthread_mutex_unlock(&g_lock);
    if (new_ptt) voice_cloud_guard_begin("ptt", request_id);
}

void wakeword_session_on_failure(const char *error)
{
    pthread_mutex_lock(&g_lock);
    g_new_experiment_mode = 0;
    pthread_mutex_unlock(&g_lock);
    finish_with_cooldown(error);
}

void wakeword_session_on_pending(void)
{
    set_state(WAKE_SESSION_WAIT_CONFIRM, NULL);
    voice_cloud_guard_set_phase(CLOUD_PHASE_WAIT_CONFIRM);
}

void wakeword_session_on_confirmed(void)
{
    pthread_mutex_lock(&g_lock);
    g_new_experiment_mode = 0;
    pthread_mutex_unlock(&g_lock);
    finish_with_cooldown_async();
}

void wakeword_session_on_tts_begin(void)
{
    pthread_mutex_lock(&g_lock);
    g_pending_during_tts = g_snapshot.state == WAKE_SESSION_WAIT_CONFIRM;
    pthread_mutex_unlock(&g_lock);
    set_state(WAKE_SESSION_TTS, NULL);
    voice_cloud_guard_set_phase(CLOUD_PHASE_TTS);
}

void wakeword_session_on_tts_end(int result)
{
    pthread_mutex_lock(&g_lock);
    int pending = g_pending_during_tts;
    g_pending_during_tts = 0;
    pthread_mutex_unlock(&g_lock);
    if (pending) {
        set_state(WAKE_SESSION_WAIT_CONFIRM,
                  result == 0 ? NULL : "confirmation prompt TTS failed");
        return;
    }
    finish_with_cooldown(result == 0 ? NULL : "TTS failed");
}

void wakeword_session_get_snapshot(wake_session_snapshot_t *snapshot)
{
    if (!snapshot) return;
    pthread_mutex_lock(&g_lock);
    *snapshot = g_snapshot;
    pthread_mutex_unlock(&g_lock);
}

void wakeword_session_prepare_agent_request(const char *transcript,
                                            char *request,
                                            size_t request_size)
{
    int new_experiment;

    if (!request || request_size == 0) return;
    pthread_mutex_lock(&g_lock);
    new_experiment = g_new_experiment_mode;
    g_new_experiment_mode = 0;
    pthread_mutex_unlock(&g_lock);

    if (new_experiment) {
        snprintf(request, request_size,
                 "【实验创建模式】用户要新建实验。请根据下列语音调用 "
                 "experiment_create；补全实验名称和 1 至 16 个可执行步骤。"
                 "信息不足时只提出澄清问题，禁止执行其他变更。语音内容：%s",
                 transcript ? transcript : "");
    } else {
        snprintf(request, request_size, "%s", transcript ? transcript : "");
    }
}

int wakeword_session_accepts_action(void)
{
    int accepted;

    pthread_mutex_lock(&g_lock);
    accepted = g_snapshot.state == WAKE_SESSION_LLM;
    pthread_mutex_unlock(&g_lock);
    return accepted;
}

int wakeword_session_tts_authorized(void)
{
    pthread_mutex_lock(&g_lock);
    int allowed = g_snapshot.state == WAKE_SESSION_LLM ||
                  g_snapshot.state == WAKE_SESSION_WAIT_CONFIRM;
    pthread_mutex_unlock(&g_lock);
    return allowed;
}

int wakeword_session_test_wake(wakeword_language_t language, float score)
{
    return wakeword_engine_test_detection(language, score);
}
