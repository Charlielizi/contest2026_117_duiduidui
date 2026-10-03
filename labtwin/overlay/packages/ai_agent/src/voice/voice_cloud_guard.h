#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CLOUD_ASR = 0, CLOUD_LLM, CLOUD_TTS, CLOUD_SERVICE_COUNT } cloud_service_t;
typedef enum { CLOUD_PHASE_IDLE = 0, CLOUD_PHASE_RECORDING, CLOUD_PHASE_ASR,
               CLOUD_PHASE_LLM, CLOUD_PHASE_WAIT_CONFIRM, CLOUD_PHASE_TTS } cloud_phase_t;
typedef enum { CLOUD_FAULT_NONE = 0, CLOUD_FAULT_NETWORK, CLOUD_FAULT_ASR,
               CLOUD_FAULT_LLM, CLOUD_FAULT_TTS } cloud_fault_t;

typedef struct {
    uint64_t allowed[CLOUD_SERVICE_COUNT];
    uint64_t denied[CLOUD_SERVICE_COUNT];
    uint64_t injected[CLOUD_SERVICE_COUNT];
    cloud_phase_t phase;
    cloud_fault_t fault;
    char source[8];
    char request_id[40];
} voice_cloud_guard_stats_t;

void voice_cloud_guard_begin(const char *source, const char *request_id);
void voice_cloud_guard_set_phase(cloud_phase_t phase);
void voice_cloud_guard_end(void);
int voice_cloud_guard_check(cloud_service_t service);
void voice_cloud_guard_get_stats(voice_cloud_guard_stats_t *stats);
int voice_cloud_guard_set_fault(cloud_fault_t fault);
const char *voice_cloud_guard_phase_name(cloud_phase_t phase);
const char *voice_cloud_guard_fault_name(cloud_fault_t fault);

#ifdef __cplusplus
}
#endif
