#include "voice/voice_action.h"
#include "voice/voice_request_ledger.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int executions;
static int slow_execution;
bool tool_registry_requires_confirmation(const char *name)
{
    return strcmp(name, "experiment_create") == 0 ||
           strcmp(name, "experiment_transition") == 0 ||
           strcmp(name, "timer_start") == 0 ||
           strcmp(name, "timer_cancel") == 0 ||
           strcmp(name, "lab_log_add") == 0 ||
           strcmp(name, "environment_event_ack") == 0 ||
           strcmp(name, "write_file") == 0;
}

int tool_registry_execute(const char *name, const char *input,
                          char *output, size_t output_size)
{
    if (slow_execution) usleep(100 * 1000);
    executions++;
    snprintf(output, output_size, "{\"ok\":true,\"tool\":\"%s\"}", name);
    (void)input;
    return 0;
}

typedef struct {
    int ret;
    char output[512];
} confirm_result_t;

static void *confirm_thread(void *arg)
{
    confirm_result_t *result = arg;
    result->ret = voice_action_confirm(result->output, sizeof(result->output));
    return NULL;
}

int main(void)
{
    const char *ledger = "/tmp/voice_action_ledger_test.bin";
    unlink(ledger);
    assert(voice_request_ledger_init(ledger) == 0);
    char out[512];
    assert(voice_action_is_mutating_tool("timer_start"));
    assert(voice_action_is_mutating_tool("environment_event_ack"));
    assert(voice_action_is_mutating_tool("write_file"));
    assert(!voice_action_is_mutating_tool("experiment_get"));
    assert(voice_action_prepare("create-1", "创建滴定实验", "experiment_create",
        "{\"name\":\"滴定实验\",\"steps\":[\"加样\",\"滴定\"]}",
        out, sizeof(out)) == 0);
    voice_action_snapshot_t snapshot;
    voice_action_get_snapshot(&snapshot);
    assert(strstr(snapshot.summary, "2 个步骤"));
    assert(voice_action_confirm(out, sizeof(out)) == 0);
    assert(executions == 1);
    assert(voice_action_prepare("bad-id", "启动计时", "timer_start",
        "{\"step_index\":0,\"label\":\"反应\",\"duration_seconds\":60}",
        out, sizeof(out)) < 0);
    assert(voice_action_prepare("bad-duration", "启动计时", "timer_start",
        "{\"experiment_id\":\"M3\",\"step_index\":0,\"label\":\"反应\",\"duration_seconds\":0}",
        out, sizeof(out)) < 0);
    assert(voice_action_prepare("r1", "启动一分钟计时", "timer_start",
        "{\"experiment_id\":\"M3\",\"step_index\":0,\"label\":\"反应\",\"duration_seconds\":60}",
        out, sizeof(out)) == 0);
    assert(strstr(out, "PENDING_CONFIRMATION"));
    assert(executions == 1);
    assert(voice_action_confirm(out, sizeof(out)) == 0);
    assert(executions == 2);
    assert(voice_action_confirm(out, sizeof(out)) == 0);
    assert(executions == 2);
    assert(voice_action_prepare("r2", "取消计时", "timer_cancel",
        "{\"experiment_id\":\"M3\",\"timer_id\":\"T1\"}",
        out, sizeof(out)) == 0);
    assert(voice_action_cancel() == 0);
    assert(voice_action_confirm(out, sizeof(out)) < 0);
    assert(executions == 2);
    assert(voice_action_prepare("bad-transition", "跳转", "experiment_transition",
        "{\"experiment_id\":\"M3\",\"action\":\"force\"}",
        out, sizeof(out)) == -EINVAL);
    assert(voice_action_prepare("r3", "确认温度告警", "environment_event_ack",
        "{\"event_id\":\"ENV-1\"}", out, sizeof(out)) == 0);
    slow_execution = 1;
    confirm_result_t first = {0};
    confirm_result_t second = {0};
    pthread_t first_thread;
    pthread_t second_thread;
    assert(pthread_create(&first_thread, NULL, confirm_thread, &first) == 0);
    usleep(10 * 1000);
    assert(pthread_create(&second_thread, NULL, confirm_thread, &second) == 0);
    pthread_join(first_thread, NULL);
    pthread_join(second_thread, NULL);
    assert(first.ret == 0);
    assert(second.ret == -EBUSY);
    assert(executions == 3);

    slow_execution = 0;
    assert(voice_action_prepare("r4", "记录终点温度", "lab_log_add",
        "{\"experiment_id\":\"M3\",\"text\":\"终点温度 25 C\"}",
        out, sizeof(out)) == 0);
    /* Persist EXECUTING, execute once, then fail the APPLIED write. */
    voice_request_ledger_fail_persist_after_for_test(1);
    assert(voice_action_confirm(out, sizeof(out)) != 0);
    assert(strstr(out, "ACTION_OUTCOME_UNKNOWN"));
    assert(executions == 4);

    /* A reboot reloads EXECUTING and must not replay the side effect. */
    voice_request_ledger_reset_for_test();
    assert(voice_request_ledger_init(ledger) == 0);
    assert(voice_action_prepare("r4", "记录终点温度", "lab_log_add",
        "{\"experiment_id\":\"M3\",\"text\":\"终点温度 25 C\"}",
        out, sizeof(out)) == 0);
    assert(strstr(out, "ACTION_OUTCOME_UNKNOWN"));
    assert(voice_action_confirm(out, sizeof(out)) == -ENOENT);
    assert(executions == 4);
    unlink(ledger);
    puts("voice_action_host_test: PASS");
    return 0;
}
