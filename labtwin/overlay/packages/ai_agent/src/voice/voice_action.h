#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_ACTION_NONE = 0,
    VOICE_ACTION_PENDING,
    VOICE_ACTION_EXECUTING,
    VOICE_ACTION_APPLIED,
    VOICE_ACTION_CANCELLED,
    VOICE_ACTION_FAILED
} voice_action_state_t;

typedef struct {
    voice_action_state_t state;
    char request_id[40];
    char transcript[512];
    char tool[40];
    char arguments[1024];
    char summary[256];
    char result[1024];
} voice_action_snapshot_t;

int voice_action_is_mutating_tool(const char *tool);
int voice_action_prepare(const char *request_id, const char *transcript,
                         const char *tool, const char *arguments,
                         char *output, size_t output_size);
int voice_action_confirm(char *output, size_t output_size);
int voice_action_cancel(void);
void voice_action_get_snapshot(voice_action_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
