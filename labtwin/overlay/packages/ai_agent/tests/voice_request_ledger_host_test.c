#include "voice/voice_request_ledger.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const char *path = "/tmp/voice_request_ledger_test.bin";
    unlink(path);
    unlink("/tmp/voice_request_ledger_test.bin.tmp");
    assert(voice_request_ledger_init(path) == 0);
    assert(voice_request_ledger_prepare("req-1", "lab_log_add",
        "{\"experiment_id\":\"E1\"}", "记录温度") == 0);
    assert(voice_request_ledger_prepare("req-1", "lab_log_add",
        "{\"experiment_id\":\"E1\"}", "记录温度") == 1);
    assert(voice_request_ledger_prepare("req-1", "timer_start",
        "{}", "记录温度") < 0);
    assert(voice_request_ledger_finish("req-1", VOICE_LEDGER_APPLIED,
        "{\"ok\":true}") == 0);

    voice_ledger_state_t state = 0;
    char result[128] = {0};
    voice_request_ledger_reset_for_test();
    assert(voice_request_ledger_init(path) == 0);
    assert(voice_request_ledger_lookup("req-1", &state, result,
                                      sizeof(result)) == 0);
    assert(state == VOICE_LEDGER_APPLIED);
    assert(strcmp(result, "{\"ok\":true}") == 0);
    assert(voice_request_ledger_finish("req-1", VOICE_LEDGER_CANCELLED,
                                      "cancel") < 0);

    /* Finalized records may be evicted, but active/uncertain records must not
     * be replaced because that would allow an old request to execute again. */
    for (int i = 2; i <= 70; i++) {
        char id[40];
        snprintf(id, sizeof(id), "req-%d", i);
        assert(voice_request_ledger_prepare(id, "lab_log_add", "{}", id) == 0);
        assert(voice_request_ledger_finish(id, VOICE_LEDGER_APPLIED,
                                          "{\"ok\":true}") == 0);
    }
    assert(voice_request_ledger_lookup("req-70", &state, NULL, 0) == 0);
    assert(voice_request_ledger_lookup("req-1", &state, NULL, 0) < 0);

    unlink(path);
    voice_request_ledger_reset_for_test();
    assert(voice_request_ledger_init(path) == 0);
    for (int i = 0; i < 64; i++) {
        char id[40];
        snprintf(id, sizeof(id), "active-%d", i);
        assert(voice_request_ledger_prepare(id, "lab_log_add", "{}", id) == 0);
    }
    assert(voice_request_ledger_prepare("active-overflow", "lab_log_add",
        "{}", "overflow") == -ENOSPC);

    unlink(path);
    FILE *corrupt = fopen(path, "w");
    assert(corrupt != NULL);
    assert(fputs("corrupt", corrupt) >= 0);
    assert(fclose(corrupt) == 0);
    voice_request_ledger_reset_for_test();
    assert(voice_request_ledger_init(path) == -EIO);

    unlink(path);
    puts("voice_request_ledger_host_test: PASS");
    return 0;
}
