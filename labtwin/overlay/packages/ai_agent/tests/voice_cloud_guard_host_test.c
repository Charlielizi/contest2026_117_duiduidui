#include "voice/voice_cloud_guard.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    voice_cloud_guard_end();
    assert(voice_cloud_guard_check(CLOUD_ASR) == -EACCES);
    assert(voice_cloud_guard_check(CLOUD_LLM) == -EACCES);
    assert(voice_cloud_guard_check(CLOUD_TTS) == -EACCES);

    voice_cloud_guard_begin("wake", "r1");
    assert(voice_cloud_guard_check(CLOUD_ASR) == 0);
    assert(voice_cloud_guard_check(CLOUD_LLM) == -EACCES);
    assert(voice_cloud_guard_check(CLOUD_TTS) == -EACCES);
    voice_cloud_guard_set_phase(CLOUD_PHASE_LLM);
    assert(voice_cloud_guard_check(CLOUD_ASR) == -EACCES);
    assert(voice_cloud_guard_check(CLOUD_LLM) == 0);
    assert(voice_cloud_guard_check(CLOUD_TTS) == 0);

    voice_cloud_guard_set_fault(CLOUD_FAULT_NETWORK);
    assert(voice_cloud_guard_check(CLOUD_LLM) == -ENETDOWN);
    voice_cloud_guard_set_fault(CLOUD_FAULT_TTS);
    assert(voice_cloud_guard_check(CLOUD_LLM) == 0);
    assert(voice_cloud_guard_check(CLOUD_TTS) == -EIO);
    voice_cloud_guard_set_fault(CLOUD_FAULT_NONE);
    voice_cloud_guard_end();
    assert(voice_cloud_guard_check(CLOUD_TTS) == -EACCES);

    voice_cloud_guard_stats_t s;
    voice_cloud_guard_get_stats(&s);
    assert(s.allowed[CLOUD_ASR] == 1);
    assert(s.denied[CLOUD_LLM] == 2);
    assert(s.injected[CLOUD_LLM] == 1);
    assert(s.injected[CLOUD_TTS] == 1);
    puts("voice_cloud_guard_host_test: PASS");
    return 0;
}
