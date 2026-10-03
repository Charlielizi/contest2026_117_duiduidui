#include "labtwin/labtwin.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void expect_ok(int ret, const char *json)
{
    assert(ret == 0);
    assert(strstr(json, "\"ok\":true") != NULL);
}

static void expect_code(int ret, const char *json, const char *code)
{
    assert(ret != 0);
    assert(strstr(json, code) != NULL);
}

int main(void)
{
    char out[16384];
    labtwin_experiment_summary_t summary;
    size_t summary_count;
    int ret;
    assert(labtwin_service_init() == 0);
    ret = labtwin_experiment_create_json(
        "{\"experiment_id\":\"M2A\",\"name\":\"stability\","
        "\"steps\":[\"prepare\",\"observe\"]}", out, sizeof(out), "test");
    expect_ok(ret, out);
    summary_count = 0;
    assert(labtwin_experiment_list_summaries(&summary, 1, &summary_count) == 0);
    assert(summary_count == 1);
    assert(strcmp(summary.experiment_id, "M2A") == 0);
    assert(summary.state == LABTWIN_STATE_READY);
    assert(!summary.has_timer);
    assert(labtwin_focus_experiment("M2A") == 0);
    assert(labtwin_focus_experiment("unknown") == -ENOENT);
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"resume\"}",
        out, sizeof(out), "test");
    expect_code(ret, out, "INVALID_STATE");
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"start\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_timer_start_json(
        "{\"experiment_id\":\"M2A\",\"step_index\":0,"
        "\"label\":\"timer-a\",\"duration_seconds\":3}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_timer_start_json(
        "{\"experiment_id\":\"M2A\",\"step_index\":0,"
        "\"label\":\"timer-b\",\"duration_seconds\":5}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    summary_count = 0;
    assert(labtwin_experiment_list_summaries(&summary, 1, &summary_count) == 0);
    assert(summary_count == 1);
    assert(summary.has_timer && !summary.timer_expired);
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"pause\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"resume\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_log_add_json(
        "{\"experiment_id\":\"M2A\",\"text\":\"color darkened\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_log_add_json(
        "{\"experiment_id\":\"M2A\",\"text\":\"color darkened\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "ALREADY_APPLIED") != NULL);
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"complete_step\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2A\",\"action\":\"complete\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_log_add_json(
        "{\"experiment_id\":\"M2A\",\"text\":\"late write\"}",
        out, sizeof(out), "test");
    expect_code(ret, out, "INVALID_STATE");
    ret = labtwin_experiment_get_json(
        "{\"experiment_id\":\"M2A\"}", out, sizeof(out));
    expect_ok(ret, out);
    assert(strstr(out, "color darkened") != NULL);
    summary_count = 0;
    assert(labtwin_experiment_list_summaries(&summary, 1, &summary_count) == 0);
    assert(summary_count == 1);
    assert(summary.state == LABTWIN_STATE_COMPLETED);
    assert(labtwin_focus_experiment("M2A") == -EINVAL);

    /* v2 task metadata is writable only while READY and stays available from
     * the disk-backed calendar query after the small RAM cache rolls over. */
    ret = labtwin_experiment_create_json(
        "{\"experiment_id\":\"M2B\",\"name\":\"calendar task\","
        "\"description\":\"initial protocol\",\"planned_start_epoch\":1800000000,"
        "\"planned_end_epoch\":1800003600,\"steps\":[\"prepare\",\"measure\"]}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "initial protocol") != NULL);
    ret = labtwin_experiment_update_json(
        "{\"experiment_id\":\"M2B\",\"name\":\"calendar task\","
        "\"description\":\"revised protocol\",\"planned_start_epoch\":1800000000,"
        "\"planned_end_epoch\":1800007200,\"steps\":[\"prepare\",\"measure\"],"
        "\"if_event_seq\":1}", out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "revised protocol") != NULL);
    ret = labtwin_experiment_update_json(
        "{\"experiment_id\":\"M2B\",\"name\":\"calendar task\","
        "\"description\":\"stale\",\"steps\":[\"prepare\"],\"if_event_seq\":1}",
        out, sizeof(out), "test");
    expect_code(ret, out, "CONFLICT");
    ret = labtwin_experiment_transition_json(
        "{\"experiment_id\":\"M2B\",\"action\":\"start\"}", out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "started_epoch") != NULL);
    ret = labtwin_timer_start_json(
        "{\"experiment_id\":\"M2B\",\"step_index\":0,"
        "\"label\":\"expiry regression\",\"duration_seconds\":1}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    sleep(2);
    ret = labtwin_experiment_get_json(
        "{\"experiment_id\":\"M2B\"}", out, sizeof(out));
    expect_ok(ret, out);
    assert(strstr(out, "expiry regression") != NULL);
    assert(strstr(out, "\"state\":\"EXPIRED\"") != NULL);
    /* The expiry worker must release the service lock after persisting the
     * event, otherwise this synchronous write would deadlock. */
    ret = labtwin_log_add_json(
        "{\"experiment_id\":\"M2B\",\"text\":\"expiry persisted\"}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    ret = labtwin_experiment_update_json(
        "{\"experiment_id\":\"M2B\",\"name\":\"locked\","
        "\"description\":\"locked\",\"steps\":[\"prepare\"],\"if_event_seq\":2}",
        out, sizeof(out), "test");
    expect_code(ret, out, "INVALID_STATE");

    /* The portal sends explicit JSON null values for an optional blank
     * schedule.  They must be treated exactly like omitted fields. */
    ret = labtwin_experiment_create_json(
        "{\"experiment_id\":\"M2NULL\",\"name\":\"portal optional schedule\","
        "\"description\":\"nullable schedule\",\"planned_start_epoch\":null,"
        "\"planned_end_epoch\":null,\"steps\":[{\"title\":\"prepare\"}]}",
        out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "\"planned_start_epoch\":null") != NULL);
    ret = labtwin_experiment_update_json(
        "{\"experiment_id\":\"M2NULL\",\"name\":\"portal optional schedule\","
        "\"description\":\"nullable schedule revised\",\"planned_start_epoch\":null,"
        "\"planned_end_epoch\":null,\"steps\":[{\"title\":\"prepare\"}],"
        "\"if_event_seq\":1}", out, sizeof(out), "test");
    expect_ok(ret, out);
    assert(strstr(out, "nullable schedule revised") != NULL);

    for (int history = 0; history < 20; history++) {
        char input[192];
        snprintf(input, sizeof(input),
                 "{\"experiment_id\":\"H%02d\",\"name\":\"history %d\","
                 "\"steps\":[\"record\"]}", history, history);
        ret = labtwin_experiment_create_json(input, out, sizeof(out), "test");
        expect_ok(ret, out);
    }
    ret = labtwin_experiment_history_json("{\"limit\":32}", out, sizeof(out));
    expect_ok(ret, out);
    assert(strstr(out, "calendar task") != NULL);
    assert(strstr(out, "history 19") != NULL);
    ret = labtwin_experiment_history_json("{\"offset\":1,\"limit\":1}", out, sizeof(out));
    expect_ok(ret, out);
    assert(strstr(out, "\"count\":") != NULL);
    printf("labtwin_host_test: PASS\n");
    return 0;
}
