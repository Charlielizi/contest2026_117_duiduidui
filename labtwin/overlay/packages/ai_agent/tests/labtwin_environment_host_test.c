#include "labtwin/labtwin.h"
#include "labtwin/labtwin_environment.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ROOT "/tmp/labtwin-m4-environment-test"

bool labtwin_clock_trusted(void)
{
    return true;
}

int labtwin_environment_resolve_experiment(const char *preferred,
                                           char *output,
                                           size_t output_size)
{
    if (!preferred || !preferred[0]) return -ENOENT;
    snprintf(output, output_size, "%s", preferred);
    return 0;
}

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void clean_store(void)
{
    unlink(ROOT "/environment/config.json.tmp");
    unlink(ROOT "/environment/snapshot.json.tmp");
    unlink(ROOT "/environment/config.json");
    unlink(ROOT "/environment/snapshot.json");
    unlink(ROOT "/environment/events.jsonl");
    unlink(ROOT "/environment/telemetry.jsonl");
    unlink(ROOT "/environment/telemetry.hourly.jsonl");
    unlink(ROOT "/environment/telemetry.hourly.tmp");
    rmdir(ROOT "/environment");
    rmdir(ROOT);
}

static void temperature_sample(float value)
{
    labtwin_environment_submit(true, value, false, 0.0f);
    labtwin_environment_process_test(now_ms());
}

static void humidity_sample(float value)
{
    labtwin_environment_submit(false, 0.0f, true, value);
    labtwin_environment_process_test(now_ms());
}

int main(void)
{
    labtwin_environment_event_view_t event;
    char output[16384];
    char input[96];
    unsigned long long telemetry_start;
    int i;

    clean_store();
    assert(labtwin_environment_init() == 0);
    labtwin_environment_set_foreground("M4A");
    telemetry_start = now_ms();
    for (i = 0; i <= 300; i++) {
        labtwin_environment_telemetry_sample_test(true, 20.0f + i * 0.1f,
                                                   true, 50.0f + i * 0.1f,
                                                   telemetry_start + i * 1000ULL);
    }
    assert(labtwin_environment_dashboard_json(output, sizeof(output)) == 0);
    assert(strstr(output, "\"last_telemetry_seq\":1") != NULL);
    assert(strstr(output, "\"temperature_avg\"") != NULL);
    /* Seven detailed days roll into the hourly archive before the next
     * five-minute point is written. */
    for (i = 1; i <= 2016; i++) {
        labtwin_environment_telemetry_sample_test(true, 21.0f,
                                                   true, 51.0f,
                                                   telemetry_start +
                                                   300000ULL + i * 300000ULL);
    }
    assert(access(ROOT "/environment/telemetry.hourly.jsonl", F_OK) == 0);
    assert(labtwin_environment_dashboard_json(output, sizeof(output)) == 0);
    assert(strstr(output, "\"last_telemetry_seq\":2017") != NULL);
    assert(labtwin_environment_inject("temperature", NAN, false) == -EINVAL);
    assert(labtwin_environment_inject("temperature", 31.0f, false) == 0);
    assert(labtwin_environment_inject(NULL, 0.0f, true) == 0);

    temperature_sample(31.0f);
    temperature_sample(29.0f);
    assert(labtwin_environment_get_active(&event) == -ENOENT);
    for (i = 0; i < 4; i++) temperature_sample(31.0f);
    assert(labtwin_environment_get_active(&event) == -ENOENT);
    temperature_sample(31.0f);
    assert(labtwin_environment_get_active(&event) == 0);
    assert(strcmp(event.rule_id, "TEMP_HIGH") == 0);
    assert(strcmp(event.experiment_id, "M4A") == 0);

    snprintf(input, sizeof(input), "{\"event_id\":\"%s\"}", event.event_id);
    assert(labtwin_environment_ack_json(input, output, sizeof(output), "touch") == 0);
    assert(strstr(output, "ACKNOWLEDGED") != NULL);
    assert(labtwin_environment_ack_json(input, output, sizeof(output), "touch") == 0);
    assert(strstr(output, "ALREADY_APPLIED") != NULL);

    for (i = 0; i < 9; i++) temperature_sample(27.0f);
    assert(labtwin_environment_get_json(input, output, sizeof(output)) == 0);
    assert(strstr(output, "ACKNOWLEDGED") != NULL);
    temperature_sample(27.0f);
    assert(labtwin_environment_get_json(input, output, sizeof(output)) == 0);
    assert(strstr(output, "RECOVERED") != NULL);

    for (i = 0; i < 5; i++) temperature_sample(31.0f);
    assert(labtwin_environment_get_active(&event) == -ENOENT);

    assert(labtwin_environment_rule_set_json(
        "{\"rule_id\":\"HUMIDITY_HIGH\",\"trigger\":65,\"clear\":68}",
        output, sizeof(output)) == -EINVAL);
    assert(labtwin_environment_rule_set_json(
        "{\"rule_id\":\"HUMIDITY_HIGH\",\"trigger\":68,\"clear\":63}",
        output, sizeof(output)) == 0);
    for (i = 0; i < 5; i++) humidity_sample(75.0f);
    assert(labtwin_environment_get_active(&event) == 0);
    assert(strcmp(event.rule_id, "HUMIDITY_HIGH") == 0);
    snprintf(input, sizeof(input), "{\"event_id\":\"%s\"}", event.event_id);
    assert(labtwin_environment_ack_json(input, output, sizeof(output), "touch") == 0);

    assert(labtwin_environment_rules_set_json(
        "{\"rules\":["
        "{\"rule_id\":\"TEMP_HIGH\",\"trigger\":32,\"clear\":29},"
        "{\"rule_id\":\"TEMP_LOW\",\"trigger\":9,\"clear\":13},"
        "{\"rule_id\":\"HUMIDITY_HIGH\",\"trigger\":72,\"clear\":66},"
        "{\"rule_id\":\"HUMIDITY_LOW\",\"trigger\":18,\"clear\":24}"
        "]}",
        output, sizeof(output)) == 0);
    assert(labtwin_environment_rule_show_json(output, sizeof(output)) == 0);
    assert(strstr(output, "\"trigger\":32") != NULL);
    assert(strstr(output, "\"trigger\":72") != NULL);
    assert(labtwin_environment_rules_set_json(
        "{\"rules\":["
        "{\"rule_id\":\"TEMP_HIGH\",\"trigger\":33,\"clear\":29},"
        "{\"rule_id\":\"TEMP_HIGH\",\"trigger\":34,\"clear\":29},"
        "{\"rule_id\":\"HUMIDITY_HIGH\",\"trigger\":72,\"clear\":66},"
        "{\"rule_id\":\"HUMIDITY_LOW\",\"trigger\":18,\"clear\":24}"
        "]}",
        output, sizeof(output)) == -EINVAL);
    assert(labtwin_environment_rule_show_json(output, sizeof(output)) == 0);
    assert(strstr(output, "\"trigger\":32") != NULL);

    labtwin_environment_shutdown_test();
    assert(labtwin_environment_init() == 0);
    assert(labtwin_environment_get_json(input, output, sizeof(output)) == 0);
    assert(strstr(output, "ACKNOWLEDGED") != NULL);
    assert(strstr(output, event.event_id) != NULL);
    assert(labtwin_environment_dashboard_json(output, sizeof(output)) == 0);
    assert(strstr(output, "\"last_telemetry_seq\":2017") != NULL);

    labtwin_environment_shutdown_test();
    clean_store();
    puts("labtwin_environment_host_test: PASS");
    return 0;
}
