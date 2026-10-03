#pragma once

#include <stddef.h>

#include "voice/wakeword_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WAKE_SESSION_DISABLED = 0,
    WAKE_SESSION_PTT_READY,
    WAKE_SESSION_LOCAL_LISTENING,
    WAKE_SESSION_WAKE_DETECTED,
    WAKE_SESSION_COMMAND_RECORDING,
    WAKE_SESSION_ASR,
    WAKE_SESSION_LLM,
    WAKE_SESSION_WAIT_CONFIRM,
    WAKE_SESSION_TTS,
    WAKE_SESSION_COOLDOWN
} wake_session_state_t;

typedef struct {
    wake_session_state_t state;
    char request_id[40];
    wakeword_language_t language;
    float score;
    char model_version[32];
    char transcript[512];
    char reply[1024];
    char error[96];
} wake_session_snapshot_t;

int wakeword_session_init(void);
void wakeword_session_shutdown(void);
int wakeword_session_start_ptt(void);
int wakeword_session_start_new_experiment_ptt(void);
int wakeword_session_cancel(void);
void wakeword_session_on_asr(const char *text);
void wakeword_session_on_partial(const char *text);
void wakeword_session_on_reply(const char *text);
void wakeword_session_on_ptt_begin(void);
void wakeword_session_on_failure(const char *error);
void wakeword_session_on_pending(void);
void wakeword_session_on_confirmed(void);
void wakeword_session_on_tts_begin(void);
void wakeword_session_on_tts_end(int result);
void wakeword_session_get_snapshot(wake_session_snapshot_t *snapshot);
void wakeword_session_prepare_agent_request(const char *transcript,
                                            char *request,
                                            size_t request_size);
int wakeword_session_accepts_action(void);
int wakeword_session_tts_authorized(void);
int wakeword_session_test_wake(wakeword_language_t language, float score);

#ifdef __cplusplus
}
#endif
