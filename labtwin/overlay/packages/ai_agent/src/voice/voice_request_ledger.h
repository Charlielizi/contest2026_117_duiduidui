#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_LEDGER_PENDING = 1,
    VOICE_LEDGER_APPLIED,
    VOICE_LEDGER_CANCELLED,
    VOICE_LEDGER_FAILED,
    /* Persisted before the side effect starts.  After a reboot this state is
     * intentionally not replayed because the previous outcome is unknown. */
    VOICE_LEDGER_EXECUTING
} voice_ledger_state_t;

int voice_request_ledger_init(const char *path);
int voice_request_ledger_prepare(const char *request_id, const char *tool,
                                 const char *arguments,
                                 const char *transcript);
int voice_request_ledger_lookup(const char *request_id,
                                voice_ledger_state_t *state,
                                char *result, size_t result_size);
int voice_request_ledger_finish(const char *request_id,
                                voice_ledger_state_t state,
                                const char *result);
void voice_request_ledger_reset_for_test(void);
#ifndef __NuttX__
void voice_request_ledger_fail_persist_after_for_test(unsigned int successes);
#endif

#ifdef __cplusplus
}
#endif
