#include "voice/wakeword_session.h"
#include "voice/audio_arbiter.h"
#include "voice/audio_playback.h"
#include "voice/voice_channel.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_voice_ready = -ENODEV;
static int g_cancel_calls;
static int g_wake_register_calls;

int wakeword_tflm_register(void)
{
    g_wake_register_calls++;
    return -ENODEV;
}
int wakeword_engine_init(wakeword_detected_cb_t cb, void *arg)
{ (void)cb; (void)arg; return -ENODEV; }
int wakeword_engine_start(void) { return -ENODEV; }
void wakeword_engine_stop(void) {}
int wakeword_engine_pause(void) { return 0; }
int wakeword_engine_resume(void) { return 0; }
int wakeword_engine_test_detection(wakeword_language_t l, float s)
{ (void)l; (void)s; return -ENODEV; }

int audio_arbiter_acquire(audio_owner_t owner)
{ (void)owner; return -EBUSY; }
void audio_arbiter_release(audio_owner_t owner) { (void)owner; }
audio_playback_t *audio_playback_open(const char *p, unsigned int r,
                                      unsigned int c, unsigned int b)
{ (void)p; (void)r; (void)c; (void)b; return NULL; }
int audio_playback_write(audio_playback_t *p, const void *d, size_t n)
{ (void)p; (void)d; (void)n; return -EIO; }
void audio_playback_close(audio_playback_t *p) { (void)p; }

int voice_channel_is_ready(void) { return g_voice_ready; }
int voice_channel_start_auto(unsigned int max_ms, unsigned int silence_ms)
{ (void)max_ms; (void)silence_ms; return 0; }
int voice_channel_auto_done(void) { return 0; }
int voice_channel_stop(void) { return 0; }
int voice_channel_cancel(void) { g_cancel_calls++; return 0; }

int main(void)
{
    wake_session_snapshot_t snapshot;
    char request[1024];

    assert(wakeword_session_init() == 0);
    assert(wakeword_session_start_new_experiment_ptt() == -ENODEV);
    assert(g_wake_register_calls == 0);
    wakeword_session_get_snapshot(&snapshot);
    assert(snapshot.state == WAKE_SESSION_PTT_READY);

    g_voice_ready = 0;
    assert(wakeword_session_start_new_experiment_ptt() == 0);
    wakeword_session_prepare_agent_request("建立酸碱滴定实验", request,
                                           sizeof(request));
    assert(strstr(request, "experiment_create") != NULL);
    assert(strstr(request, "建立酸碱滴定实验") != NULL);

    wakeword_session_on_partial("建立酸碱");
    wakeword_session_get_snapshot(&snapshot);
    assert(strcmp(snapshot.transcript, "建立酸碱") == 0);
    wakeword_session_on_asr("建立酸碱滴定实验");
    wakeword_session_on_reply("请确认创建实验");
    wakeword_session_on_partial("late partial must not replace final");
    wakeword_session_get_snapshot(&snapshot);
    assert(strcmp(snapshot.transcript, "建立酸碱滴定实验") == 0);
    assert(strcmp(snapshot.reply, "请确认创建实验") == 0);
    assert(wakeword_session_accepts_action());
    assert(wakeword_session_cancel() == 0);
    assert(!wakeword_session_accepts_action());
    usleep(20 * 1000);
    assert(g_cancel_calls == 1);
    wakeword_session_get_snapshot(&snapshot);
    assert(strcmp(snapshot.reply, "请确认创建实验") == 0);
    puts("wakeword_session_host_test: PASS");
    return 0;
}
