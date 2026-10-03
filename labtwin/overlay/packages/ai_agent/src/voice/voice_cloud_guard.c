#include "voice/voice_cloud_guard.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_guard_lock = PTHREAD_MUTEX_INITIALIZER;
static voice_cloud_guard_stats_t g_guard;

void voice_cloud_guard_begin(const char *source, const char *request_id)
{
    pthread_mutex_lock(&g_guard_lock);
    g_guard.phase = CLOUD_PHASE_RECORDING;
    snprintf(g_guard.source, sizeof(g_guard.source), "%s", source ? source : "unknown");
    snprintf(g_guard.request_id, sizeof(g_guard.request_id), "%s",
             request_id ? request_id : "");
    pthread_mutex_unlock(&g_guard_lock);
}

void voice_cloud_guard_set_phase(cloud_phase_t phase)
{
    pthread_mutex_lock(&g_guard_lock);
    if (g_guard.phase != CLOUD_PHASE_IDLE || phase == CLOUD_PHASE_IDLE)
        g_guard.phase = phase;
    pthread_mutex_unlock(&g_guard_lock);
}

void voice_cloud_guard_end(void)
{
    pthread_mutex_lock(&g_guard_lock);
    g_guard.phase = CLOUD_PHASE_IDLE;
    g_guard.source[0] = 0;
    g_guard.request_id[0] = 0;
    pthread_mutex_unlock(&g_guard_lock);
}

static int phase_allows(cloud_phase_t phase, cloud_service_t service)
{
    if (service == CLOUD_ASR)
        return phase == CLOUD_PHASE_RECORDING || phase == CLOUD_PHASE_ASR;
    if (service == CLOUD_LLM)
        return phase == CLOUD_PHASE_LLM;
    if (service == CLOUD_TTS)
        return phase == CLOUD_PHASE_LLM || phase == CLOUD_PHASE_WAIT_CONFIRM ||
               phase == CLOUD_PHASE_TTS;
    return 0;
}

int voice_cloud_guard_check(cloud_service_t service)
{
    if ((unsigned)service >= CLOUD_SERVICE_COUNT) return -EINVAL;
    pthread_mutex_lock(&g_guard_lock);
    if (!phase_allows(g_guard.phase, service)) {
        g_guard.denied[service]++;
        pthread_mutex_unlock(&g_guard_lock);
        return -EACCES;
    }
    int inject = g_guard.fault == CLOUD_FAULT_NETWORK ||
        (service == CLOUD_ASR && g_guard.fault == CLOUD_FAULT_ASR) ||
        (service == CLOUD_LLM && g_guard.fault == CLOUD_FAULT_LLM) ||
        (service == CLOUD_TTS && g_guard.fault == CLOUD_FAULT_TTS);
    if (inject) {
        g_guard.injected[service]++;
        pthread_mutex_unlock(&g_guard_lock);
        return g_guard.fault == CLOUD_FAULT_NETWORK ? -ENETDOWN : -EIO;
    }
    g_guard.allowed[service]++;
    pthread_mutex_unlock(&g_guard_lock);
    return 0;
}

void voice_cloud_guard_get_stats(voice_cloud_guard_stats_t *stats)
{
    if (!stats) return;
    pthread_mutex_lock(&g_guard_lock);
    *stats = g_guard;
    pthread_mutex_unlock(&g_guard_lock);
}

int voice_cloud_guard_set_fault(cloud_fault_t fault)
{
    if ((unsigned)fault > CLOUD_FAULT_TTS) return -EINVAL;
    pthread_mutex_lock(&g_guard_lock);
    g_guard.fault = fault;
    pthread_mutex_unlock(&g_guard_lock);
    return 0;
}

const char *voice_cloud_guard_phase_name(cloud_phase_t phase)
{
    static const char *const names[] = {"idle", "recording", "asr", "llm", "wait_confirm", "tts"};
    return (unsigned)phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "unknown";
}

const char *voice_cloud_guard_fault_name(cloud_fault_t fault)
{
    static const char *const names[] = {"none", "network", "asr", "llm", "tts"};
    return (unsigned)fault < sizeof(names) / sizeof(names[0]) ? names[fault] : "unknown";
}
