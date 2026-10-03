#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_OWNER_NONE = 0,
    AUDIO_OWNER_WAKEWORD,
    AUDIO_OWNER_ASR,
    AUDIO_OWNER_TTS,
    AUDIO_OWNER_PTT,
    AUDIO_OWNER_ALERT,
    AUDIO_OWNER_RECORDING
} audio_owner_t;

int audio_arbiter_acquire(audio_owner_t owner);
void audio_arbiter_release(audio_owner_t owner);
audio_owner_t audio_arbiter_owner(void);
const char *audio_arbiter_owner_name(audio_owner_t owner);

#ifdef __cplusplus
}
#endif
