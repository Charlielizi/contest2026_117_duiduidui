#include "voice/audio_arbiter.h"

#include <errno.h>
#include <pthread.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static audio_owner_t g_owner;

int audio_arbiter_acquire(audio_owner_t owner)
{
    int ret = 0;
    if (owner == AUDIO_OWNER_NONE) return -EINVAL;
    pthread_mutex_lock(&g_lock);
    if (g_owner != AUDIO_OWNER_NONE && g_owner != owner) ret = -EBUSY;
    else g_owner = owner;
    pthread_mutex_unlock(&g_lock);
    return ret;
}

void audio_arbiter_release(audio_owner_t owner)
{
    pthread_mutex_lock(&g_lock);
    if (g_owner == owner) g_owner = AUDIO_OWNER_NONE;
    pthread_mutex_unlock(&g_lock);
}

audio_owner_t audio_arbiter_owner(void)
{
    audio_owner_t owner;
    pthread_mutex_lock(&g_lock);
    owner = g_owner;
    pthread_mutex_unlock(&g_lock);
    return owner;
}

const char *audio_arbiter_owner_name(audio_owner_t owner)
{
    switch (owner) {
    case AUDIO_OWNER_WAKEWORD: return "wakeword";
    case AUDIO_OWNER_ASR: return "asr";
    case AUDIO_OWNER_TTS: return "tts";
    case AUDIO_OWNER_PTT: return "ptt";
    case AUDIO_OWNER_ALERT: return "alert";
    case AUDIO_OWNER_RECORDING: return "recording";
    default: return "none";
    }
}
