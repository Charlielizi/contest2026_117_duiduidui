#include "labtwin/labtwin_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *protocol_v1 =
    "{"
    "\"schema_version\":1,"
    "\"protocol_id\":\"TEMP-STABILITY\","
    "\"version\":1,"
    "\"name\":\"Temperature stability\","
    "\"description\":\"Observe a sample after a stable dwell\","
    "\"parameters\":["
      "{\"name\":\"target_c\",\"type\":\"number\",\"unit\":\"C\","
       "\"required\":true,\"minimum\":0,\"maximum\":80}"
    "],"
    "\"prerequisites\":["
      "{\"type\":\"manual_confirm\",\"prompt\":\"Sample label checked\"}"
    "],"
    "\"steps\":["
      "{\"step_id\":\"dwell\",\"title\":\"Wait for stability\","
       "\"instructions\":\"Keep the sample in the chamber\","
       "\"prerequisites\":[],"
       "\"completion_condition\":{\"type\":\"timer_elapsed\","
         "\"duration_seconds\":60},"
       "\"timeout_seconds\":120,\"on_timeout\":\"ask_operator\","
       "\"on_failure\":\"pause\",\"risk_level\":\"low\"},"
      "{\"step_id\":\"observe\",\"title\":\"Record appearance\","
       "\"completion_condition\":{\"type\":\"observation_required\","
         "\"key\":\"appearance\"},"
       "\"on_failure\":\"ask_operator\",\"risk_level\":\"low\"}"
    "],"
    "\"acceptance_criteria\":["
      "{\"type\":\"sensor_range\",\"sensor\":\"temperature\","
       "\"unit\":\"C\",\"minimum\":20,\"maximum\":30,"
       "\"stable_for_seconds\":30,\"freshness_ms\":5000}"
    "]"
    "}";

static const char *protocol_v2 =
    "{"
    "\"schema_version\":1,"
    "\"protocol_id\":\"TEMP-STABILITY\","
    "\"version\":2,"
    "\"name\":\"Temperature stability revised\","
    "\"parameters\":[],\"prerequisites\":[],"
    "\"steps\":[{\"step_id\":\"check\",\"title\":\"Check sample\","
      "\"completion_condition\":{\"type\":\"manual_confirm\"},"
      "\"timeout_seconds\":60,\"on_timeout\":\"retry\",\"retry_limit\":2,"
      "\"risk_level\":\"medium\"}],"
    "\"acceptance_criteria\":["
      "{\"type\":\"tool_result\",\"tool\":\"sensor_snapshot\","
       "\"field\":\"temperature_available\",\"operator\":\"eq\","
       "\"value\":true}"
    "]"
    "}";

static void expect_ok(int result, const char *output)
{
    assert(result == 0);
    assert(strstr(output, "\"ok\":true") != NULL);
}

static void expect_code(int result, const char *output, const char *code)
{
    assert(result != 0);
    assert(strstr(output, code) != NULL);
}

int main(void)
{
    char output[32768];
    int result;

    assert(labtwin_protocol_init() == 0);

    expect_ok(labtwin_protocol_validate_json(protocol_v1, output,
                                             sizeof(output)), output);
    result = labtwin_protocol_validate_json(
        "{\"schema_version\":1,\"protocol_id\":\"BAD\",\"version\":1,"
        "\"name\":\"bad\",\"parameters\":[],\"prerequisites\":[],"
        "\"steps\":[{\"step_id\":\"x\",\"title\":\"x\","
        "\"completion_condition\":{\"type\":\"free_form_llm\"}}],"
        "\"acceptance_criteria\":[{\"type\":\"manual_confirm\"}]}",
        output, sizeof(output));
    expect_code(result, output, "INVALID_PROTOCOL");

    result = labtwin_protocol_validate_json(
        "{\"schema_version\":1,\"protocol_id\":\"BAD\",\"version\":1,"
        "\"name\":\"bad\",\"parameters\":[],\"prerequisites\":[],"
        "\"steps\":[{\"step_id\":\"same\",\"title\":\"one\","
        "\"completion_condition\":{\"type\":\"manual_confirm\"}},"
        "{\"step_id\":\"same\",\"title\":\"two\","
        "\"completion_condition\":{\"type\":\"manual_confirm\"}}],"
        "\"acceptance_criteria\":[{\"type\":\"manual_confirm\"}]}",
        output, sizeof(output));
    expect_code(result, output, "INVALID_PROTOCOL");

    expect_ok(labtwin_protocol_create_json(protocol_v1, output,
                                           sizeof(output), "test"), output);
    assert(strstr(output, "Temperature stability") != NULL);
    result = labtwin_protocol_create_json(protocol_v1, output,
                                          sizeof(output), "test");
    expect_code(result, output, "VERSION_CONFLICT");

    result = labtwin_protocol_create_json(
        "{\"schema_version\":1,\"protocol_id\":\"TEMP-STABILITY\","
        "\"version\":3,\"name\":\"skip\",\"parameters\":[],"
        "\"prerequisites\":[],\"steps\":[{\"step_id\":\"x\","
        "\"title\":\"x\",\"completion_condition\":{\"type\":"
        "\"manual_confirm\"}}],\"acceptance_criteria\":[{\"type\":"
        "\"manual_confirm\"}]}", output, sizeof(output), "test");
    expect_code(result, output, "VERSION_CONFLICT");

    expect_ok(labtwin_protocol_create_json(protocol_v2, output,
                                           sizeof(output), "test"), output);
    expect_ok(labtwin_protocol_get_json(
        "{\"protocol_id\":\"TEMP-STABILITY\",\"version\":1}", output,
        sizeof(output)), output);
    assert(strstr(output, "Temperature stability revised") == NULL);
    assert(strstr(output, "Temperature stability") != NULL);
    expect_ok(labtwin_protocol_get_json(
        "{\"protocol_id\":\"TEMP-STABILITY\",\"version\":2}", output,
        sizeof(output)), output);
    assert(strstr(output, "Temperature stability revised") != NULL);

    expect_ok(labtwin_protocol_list_json(
        "{\"protocol_id\":\"TEMP-STABILITY\",\"limit\":10}", output,
        sizeof(output)), output);
    assert(strstr(output, "\"count\":2") != NULL);
    assert(strstr(output, "\"version\":1") != NULL);
    assert(strstr(output, "\"version\":2") != NULL);

    {
        FILE *corrupt = fopen(
            "/tmp/labtwin-protocol-v1-test/protocols/TEMP-STABILITY.v2.json", "w");
        assert(corrupt);
        assert(fputs("{\"schema_version\":1,\"protocol_id\":\"TEMP-STABILITY\","
                     "\"version\":2}\n", corrupt) >= 0);
        assert(fclose(corrupt) == 0);
    }
    result = labtwin_protocol_get_json(
        "{\"protocol_id\":\"TEMP-STABILITY\",\"version\":2}", output,
        sizeof(output));
    expect_code(result, output, "NOT_FOUND");

    result = labtwin_protocol_get_json(
        "{\"protocol_id\":\"TEMP-STABILITY\",\"version\":9}", output,
        sizeof(output));
    expect_code(result, output, "NOT_FOUND");

    puts("labtwin_protocol_host_test: PASS");
    return 0;
}
