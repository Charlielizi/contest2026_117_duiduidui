#include "labtwin/labtwin_controller.h"
#include "labtwin/labtwin.h"
#include "labtwin/labtwin_protocol.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_SIZE 32768

static const char *protocol_json =
    "{\"schema_version\":1,\"protocol_id\":\"CLOSED-LOOP\",\"version\":1,"
    "\"name\":\"Closed loop test\","
    "\"parameters\":[{\"name\":\"target\",\"type\":\"number\","
      "\"required\":true,\"minimum\":20,\"maximum\":30}],"
    "\"prerequisites\":[{\"type\":\"manual_confirm\"}],"
    "\"steps\":["
      "{\"step_id\":\"wait\",\"title\":\"Wait\","
       "\"completion_condition\":{\"type\":\"timer_elapsed\","
         "\"duration_seconds\":60},\"timeout_seconds\":120,"
       "\"on_timeout\":\"ask_operator\",\"on_failure\":\"retry\","
       "\"retry_limit\":1,\"risk_level\":\"low\"},"
      "{\"step_id\":\"record\",\"title\":\"Record\","
       "\"completion_condition\":{\"type\":\"observation_required\","
         "\"key\":\"appearance\"},\"on_failure\":\"retry\","
       "\"retry_limit\":1,\"risk_level\":\"medium\"}],"
    "\"acceptance_criteria\":["
      "{\"type\":\"sensor_range\",\"sensor\":\"temperature\","
       "\"unit\":\"C\",\"minimum\":20,\"maximum\":30,"
       "\"stable_for_seconds\":10,\"freshness_ms\":1000},"
      "{\"type\":\"tool_result\",\"tool\":\"quality\","
       "\"field\":\"score\",\"operator\":\"gte\",\"value\":0.9}]}";

static void expect_ok(int result, const char *output)
{
    if (result != 0) fprintf(stderr, "unexpected: %s\n", output);
    assert(result == 0);
    assert(strstr(output, "\"ok\":true") != NULL);
}

static void expect_code(int result, const char *output, const char *code)
{
    assert(result != 0);
    assert(strstr(output, code) != NULL);
}

static unsigned int sequence_of(const char *output)
{
    const char *position = strstr(output, "\"last_seq\":");
    assert(position);
    return (unsigned int)strtoul(position + strlen("\"last_seq\":"), NULL, 10);
}

static void command(char *output, unsigned int *sequence, const char *tail)
{
    char input[4096];
    snprintf(input, sizeof(input),
             "{\"run_id\":\"RUN-1\",\"expected_seq\":%u,%s}",
             *sequence, tail);
    expect_ok(labtwin_controller_command_json(input, output, OUT_SIZE, "test"),
              output);
    *sequence = sequence_of(output);
}

int main(void)
{
    char *output = malloc(OUT_SIZE);
    unsigned int sequence;
    int result;

    assert(output);
    assert(labtwin_service_init() == 0);
    expect_ok(labtwin_protocol_create_json(protocol_json, output, OUT_SIZE,
                                           "test"), output);
    result = labtwin_controller_create_json(
        "{\"run_id\":\"BAD\",\"protocol_id\":\"CLOSED-LOOP\","
        "\"protocol_version\":1,\"parameters\":{}}",
        output, OUT_SIZE, "test");
    expect_code(result, output, "INVALID_PARAMETERS");

    expect_ok(labtwin_experiment_create_json(
        "{\"experiment_id\":\"EXP-MISMATCH\",\"name\":\"Mismatch\","
        "\"steps\":[\"Unexpected\"]}", output, OUT_SIZE, "test"), output);
    result = labtwin_controller_create_json(
        "{\"run_id\":\"RUN-MISMATCH\",\"experiment_id\":\"EXP-MISMATCH\","
        "\"protocol_id\":\"CLOSED-LOOP\",\"protocol_version\":1,"
        "\"parameters\":{\"target\":25}}",
        output, OUT_SIZE, "test");
    expect_code(result, output, "EXPERIMENT_SYNC_FAILED");

    expect_ok(labtwin_controller_create_json(
        "{\"run_id\":\"RUN-1\",\"experiment_id\":\"EXP-1\","
        "\"protocol_id\":\"CLOSED-LOOP\",\"protocol_version\":1,"
        "\"parameters\":{\"target\":25}}",
        output, OUT_SIZE, "test"), output);
    assert(strstr(output, "\"phase\":\"PLAN\"") != NULL);
    sequence = sequence_of(output);
    expect_ok(labtwin_experiment_get_json("{\"experiment_id\":\"EXP-1\"}",
                                          output, OUT_SIZE), output);
    assert(strstr(output, "\"title\":\"Wait\"") != NULL);
    assert(strstr(output, "\"title\":\"Record\"") != NULL);
    assert(strstr(output, "\"state\":\"READY\"") != NULL);

    result = labtwin_controller_command_json(
        "{\"run_id\":\"RUN-1\",\"expected_seq\":99,\"action\":\"plan\","
        "\"evidence\":{\"manual_confirmed\":true}}",
        output, OUT_SIZE, "test");
    expect_code(result, output, "SEQUENCE_CONFLICT");

    command(output, &sequence,
            "\"action\":\"plan\",\"evidence\":{\"manual_confirmed\":true}");
    assert(strstr(output, "\"phase\":\"EXECUTE\"") != NULL);
    expect_ok(labtwin_experiment_get_json("{\"experiment_id\":\"EXP-1\"}",
                                          output, OUT_SIZE), output);
    assert(strstr(output, "\"state\":\"RUNNING\"") != NULL);
    command(output, &sequence, "\"action\":\"execute\"");
    assert(strstr(output, "\"phase\":\"OBSERVE\"") != NULL);
    command(output, &sequence,
            "\"action\":\"observe\",\"evidence\":{\"elapsed_seconds\":30}");
    assert(strstr(output, "\"phase\":\"EVALUATE\"") != NULL);
    command(output, &sequence, "\"action\":\"evaluate\"");
    assert(strstr(output, "timer has not elapsed") != NULL);
    assert(strstr(output, "\"phase\":\"OBSERVE\"") != NULL);

    command(output, &sequence,
            "\"action\":\"observe\",\"evidence\":{\"elapsed_seconds\":60}");
    command(output, &sequence, "\"action\":\"evaluate\"");
    assert(strstr(output, "\"current_step\":1") != NULL);
    expect_ok(labtwin_experiment_get_json("{\"experiment_id\":\"EXP-1\"}",
                                          output, OUT_SIZE), output);
    assert(strstr(output, "\"current_step\":1") != NULL);
    command(output, &sequence, "\"action\":\"execute\"");
    command(output, &sequence,
            "\"action\":\"failure\",\"reason\":\"camera retry\"");
    assert(strstr(output, "\"attempt\":1") != NULL);
    assert(strstr(output, "\"phase\":\"EXECUTE\"") != NULL);
    command(output, &sequence, "\"action\":\"execute\"");
    command(output, &sequence,
            "\"action\":\"observe\",\"evidence\":{"
            "\"observations\":{\"appearance\":\"clear\"}}" );
    command(output, &sequence, "\"action\":\"evaluate\"");
    assert(strstr(output, "\"scope\":\"acceptance\"") != NULL);

    command(output, &sequence,
            "\"action\":\"observe\",\"evidence\":{"
            "\"sensors\":{\"temperature\":{\"value\":25,\"unit\":\"C\","
            "\"stable_for_seconds\":10,\"age_ms\":200}},"
            "\"tool_results\":{\"quality\":{\"score\":0.95}}}" );
    command(output, &sequence, "\"action\":\"evaluate\"");
    assert(strstr(output, "\"phase\":\"COMPLETED\"") != NULL);
    expect_ok(labtwin_experiment_get_json("{\"experiment_id\":\"EXP-1\"}",
                                          output, OUT_SIZE), output);
    assert(strstr(output, "\"state\":\"COMPLETED\"") != NULL);

    remove("/tmp/labtwin-controller-v1-test/controllers/RUN-1.json");
    {
        FILE *corrupt = fopen(
            "/tmp/labtwin-controller-v1-test/controllers/RUN-1.events.jsonl", "a");
        assert(corrupt);
        assert(fputs("{\"event_type\":\"CORRUPT\",\"state\":{\"last_seq\":999}}\n",
                     corrupt) >= 0);
        assert(fclose(corrupt) == 0);
    }
    expect_ok(labtwin_controller_get_json("{\"run_id\":\"RUN-1\"}",
                                          output, OUT_SIZE), output);
    assert(strstr(output, "\"phase\":\"COMPLETED\"") != NULL);

    expect_ok(labtwin_controller_create_json(
        "{\"run_id\":\"RUN-2\",\"protocol_id\":\"CLOSED-LOOP\","
        "\"protocol_version\":1,\"parameters\":{\"target\":24}}",
        output, OUT_SIZE, "test"), output);
    sequence = sequence_of(output);
    {
        char input[512];
        snprintf(input, sizeof(input),
                 "{\"run_id\":\"RUN-2\",\"expected_seq\":%u,"
                 "\"action\":\"plan\",\"evidence\":{\"manual_confirmed\":true}}",
                 sequence);
        expect_ok(labtwin_controller_command_json(input, output, OUT_SIZE, "test"), output);
        sequence = sequence_of(output);
        snprintf(input, sizeof(input),
                 "{\"run_id\":\"RUN-2\",\"expected_seq\":%u,\"action\":\"execute\"}",
                 sequence);
        expect_ok(labtwin_controller_command_json(input, output, OUT_SIZE, "test"), output);
        sequence = sequence_of(output);
        snprintf(input, sizeof(input),
                 "{\"run_id\":\"RUN-2\",\"expected_seq\":%u,\"action\":\"timeout\"}",
                 sequence);
        expect_ok(labtwin_controller_command_json(input, output, OUT_SIZE, "test"), output);
        assert(strstr(output, "\"phase\":\"NEEDS_HELP\"") != NULL);
        sequence = sequence_of(output);
        snprintf(input, sizeof(input),
                 "{\"run_id\":\"RUN-2\",\"expected_seq\":%u,\"action\":\"resume\"}",
                 sequence);
        expect_ok(labtwin_controller_command_json(input, output, OUT_SIZE, "test"), output);
        assert(strstr(output, "\"phase\":\"OBSERVE\"") != NULL);
    }
    free(output);
    puts("labtwin_controller_host_test: PASS");
    return 0;
}
