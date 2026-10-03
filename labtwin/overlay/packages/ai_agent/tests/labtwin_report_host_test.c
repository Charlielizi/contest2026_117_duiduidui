#include "labtwin/labtwin.h"
#include "labtwin/labtwin_report.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void create_report(const char *title, const char *summary,
                          const char *supersedes, char *output, size_t size)
{
    char input[2048];
    snprintf(input, sizeof(input),
        "{\"title\":\"%s\",\"summary\":\"%s\",\"tags\":[\"HPLC\",\"筛选\"],"
        "\"experiment_id\":\"exp-a\",\"metrics\":[{\"key\":\"purity\","
        "\"value\":98.4,\"unit\":\"%%\",\"condition\":\"室温，n=3\"}]%s}",
        title, summary,
        supersedes ? ",\"supersedes_report_id\":\"rplaceholder\"" : "");
    if (supersedes) {
        char *marker = strstr(input, "rplaceholder");
        assert(marker);
        snprintf(marker, 32, "%s\"}", supersedes);
    }
    assert(labtwin_report_create_json(input, output, size, "host-test") == OK);
}

static void report_id_from(const char *json, char *id, size_t id_size)
{
    const char *start = strstr(json, "\"report_id\":\"");
    assert(start);
    start += strlen("\"report_id\":\"");
    const char *end = strchr(start, '\"');
    assert(end && (size_t)(end - start) < id_size);
    memcpy(id, start, (size_t)(end - start));
    id[end - start] = '\0';
}

int main(void)
{
    char output[8192];
    char id[32];
    const char *experiment =
        "{\"experiment_id\":\"exp-a\",\"name\":\"HPLC 筛选\","
        "\"steps\":[\"进样\"]}";

    assert(labtwin_service_init() == 0);
    assert(labtwin_experiment_create_json(experiment, output, sizeof(output),
        "host-test") == 0);
    assert(labtwin_report_init() == OK);
    create_report("HPLC 首轮", "纯度达到候选阈值。", NULL, output, sizeof(output));
    report_id_from(output, id, sizeof(id));

    assert(labtwin_report_search_json(
        "{\"query\":\"HPLC\",\"metric_key\":\"purity\",\"min_value\":95,"
        "\"sort\":\"metric_desc\"}", output, sizeof(output)) == OK);
    assert(strstr(output, id) != NULL);
    assert(strstr(output, "室温，n=3") != NULL);
    assert(labtwin_report_search_json(
        "{\"query\":\"筛选\",\"metric_key\":\"purity\",\"max_value\":98.4}",
        output, sizeof(output)) == OK);
    assert(strstr(output, id) != NULL);
    {
        char get_input[96];
        snprintf(get_input, sizeof(get_input), "{\"report_id\":\"%s\"}", id);
        assert(labtwin_report_get_json(get_input, output, sizeof(output)) == OK);
        assert(strstr(output, "98.4") != NULL);
    }

    create_report("HPLC 修订", "修订后的纯度结论。", id, output, sizeof(output));
    assert(labtwin_report_search_json(
        "{\"query\":\"首轮\",\"metric_key\":\"purity\"}",
        output, sizeof(output)) == OK);
    assert(strstr(output, id) == NULL);
    assert(labtwin_report_create_json(
        "{\"title\":\"坏报告\",\"summary\":\"x\",\"tags\":[],"
        "\"experiment_id\":\"missing\",\"metrics\":[{\"key\":\"x\",\"value\":1}]}",
        output, sizeof(output), "host-test") == ERROR);
    assert(labtwin_report_create_json(
        "{\"title\":\"缺少条件\",\"summary\":\"x\",\"tags\":[],"
        "\"experiment_id\":\"exp-a\",\"metrics\":[{\"key\":\"x\",\"value\":1,\"unit\":\"%\"}]}",
        output, sizeof(output), "host-test") == ERROR);
    assert(labtwin_report_create_json(
        "{\"title\":\"错误修订\",\"summary\":\"x\",\"tags\":[],"
        "\"metrics\":[{\"key\":\"x\",\"value\":1,\"unit\":\"%\",\"condition\":\"n=1\"}],"
        "\"supersedes_report_id\":\"not-found\"}",
        output, sizeof(output), "host-test") == ERROR);

    for (int i = 0; i < 30; i++) {
        char title[48];
        snprintf(title, sizeof(title), "容量报告 %02d", i);
        create_report(title, "容量边界。", NULL, output, sizeof(output));
    }
    assert(labtwin_report_create_json(
        "{\"title\":\"超限报告\",\"summary\":\"x\",\"tags\":[],"
        "\"metrics\":[{\"key\":\"x\",\"value\":1,\"unit\":\"%\",\"condition\":\"n=1\"}]}",
        output, sizeof(output), "host-test") == ERROR);

    puts("labtwin_report_host_test: PASS");
    return 0;
}
