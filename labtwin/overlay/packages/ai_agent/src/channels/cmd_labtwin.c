#include "channels/cmd_labtwin.h"
#include "labtwin/labtwin.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(void)
{
    printf("lab create <id|-> <name> <step1|step2>\n"
           "lab get <id> | list [state]\n"
           "lab start|pause|resume|step-complete|complete|cancel <id>\n"
           "lab timer-start <id> <step> <seconds> <label>\n"
           "lab timer-cancel <id> <timer_id>\n"
           "lab log <id> <text> | sensor | recover <id>\n"
           "lab env status | events [active|all] | get <event_id>\n"
           "lab env ack <event_id> | rule show|reset\n"
           "lab env rule set <rule_id> <trigger> <clear>\n"
#ifdef CONFIG_AI_AGENT_LABTWIN_ENV_TEST
           "lab env inject temperature|humidity <value> | inject clear\n"
#endif
           );
}

static char *json_text(cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

void cmd_labtwin(int argc, char **argv)
{
    char output[8192];
    char *input = NULL;
    cJSON *root = cJSON_CreateObject();
    const char *action = NULL;
    int ret = -1;
    if (argc < 2) { cJSON_Delete(root); print_usage(); return; }
    labtwin_service_init();
    if (!strcmp(argv[1], "create") && argc >= 5) {
        char *save = NULL;
        char *step;
        cJSON *steps;
        if (strcmp(argv[2], "-")) cJSON_AddStringToObject(root, "experiment_id", argv[2]);
        cJSON_AddStringToObject(root, "name", argv[3]);
        steps = cJSON_AddArrayToObject(root, "steps");
        step = strtok_r(argv[4], "|", &save);
        while (step) { cJSON_AddItemToArray(steps, cJSON_CreateString(step)); step = strtok_r(NULL, "|", &save); }
        input = json_text(root);
        ret = labtwin_experiment_create_json(input, output, sizeof(output), "cli");
    } else if (!strcmp(argv[1], "get") && argc >= 3) {
        cJSON_AddStringToObject(root, "experiment_id", argv[2]); input = json_text(root);
        ret = labtwin_experiment_get_json(input, output, sizeof(output));
    } else if (!strcmp(argv[1], "list")) {
        if (argc >= 3) cJSON_AddStringToObject(root, "state", argv[2]);
        input = json_text(root);
        ret = labtwin_experiment_list_json(input, output, sizeof(output));
    } else if ((!strcmp(argv[1], "start") || !strcmp(argv[1], "pause") ||
                !strcmp(argv[1], "resume") || !strcmp(argv[1], "step-complete") ||
                !strcmp(argv[1], "complete") || !strcmp(argv[1], "cancel") ||
                !strcmp(argv[1], "recover")) && argc >= 3) {
        action = !strcmp(argv[1], "step-complete") ? "complete_step" :
                 !strcmp(argv[1], "recover") ? "resume" : argv[1];
        cJSON_AddStringToObject(root, "experiment_id", argv[2]);
        cJSON_AddStringToObject(root, "action", action); input = json_text(root);
        ret = labtwin_experiment_transition_json(input, output, sizeof(output), "cli");
    } else if (!strcmp(argv[1], "timer-start") && argc >= 6) {
        cJSON_AddStringToObject(root, "experiment_id", argv[2]);
        cJSON_AddNumberToObject(root, "step_index", strtol(argv[3], NULL, 10));
        cJSON_AddNumberToObject(root, "duration_seconds", strtol(argv[4], NULL, 10));
        cJSON_AddStringToObject(root, "label", argv[5]); input = json_text(root);
        ret = labtwin_timer_start_json(input, output, sizeof(output), "cli");
    } else if (!strcmp(argv[1], "timer-cancel") && argc >= 4) {
        cJSON_AddStringToObject(root, "experiment_id", argv[2]);
        cJSON_AddStringToObject(root, "timer_id", argv[3]); input = json_text(root);
        ret = labtwin_timer_cancel_json(input, output, sizeof(output), "cli");
    } else if (!strcmp(argv[1], "log") && argc >= 4) {
        cJSON_AddStringToObject(root, "experiment_id", argv[2]);
        cJSON_AddStringToObject(root, "text", argv[3]); input = json_text(root);
        ret = labtwin_log_add_json(input, output, sizeof(output), "cli");
    } else if (!strcmp(argv[1], "env") && argc >= 3) {
        if (!strcmp(argv[2], "status")) {
            cJSON_Delete(root); root = NULL;
            ret = labtwin_environment_status_json(output, sizeof(output));
        } else if (!strcmp(argv[2], "events")) {
            if (argc >= 4) cJSON_AddStringToObject(root, "state", argv[3]);
            input = json_text(root); root = NULL;
            ret = labtwin_environment_events_json(input, output, sizeof(output));
        } else if (!strcmp(argv[2], "get") && argc >= 4) {
            cJSON_AddStringToObject(root, "event_id", argv[3]);
            input = json_text(root); root = NULL;
            ret = labtwin_environment_get_json(input, output, sizeof(output));
        } else if (!strcmp(argv[2], "ack") && argc >= 4) {
            cJSON_AddStringToObject(root, "event_id", argv[3]);
            input = json_text(root); root = NULL;
            ret = labtwin_environment_ack_json(input, output, sizeof(output), "cli");
        } else if (!strcmp(argv[2], "rule") && argc >= 4 &&
                   !strcmp(argv[3], "show")) {
            cJSON_Delete(root); root = NULL;
            ret = labtwin_environment_rule_show_json(output, sizeof(output));
        } else if (!strcmp(argv[2], "rule") && argc >= 4 &&
                   !strcmp(argv[3], "reset")) {
            cJSON_Delete(root); root = NULL;
            ret = labtwin_environment_rule_reset_json(output, sizeof(output));
        } else if (!strcmp(argv[2], "rule") && argc >= 7 &&
                   !strcmp(argv[3], "set")) {
            cJSON_AddStringToObject(root, "rule_id", argv[4]);
            cJSON_AddNumberToObject(root, "trigger", strtof(argv[5], NULL));
            cJSON_AddNumberToObject(root, "clear", strtof(argv[6], NULL));
            input = json_text(root); root = NULL;
            ret = labtwin_environment_rule_set_json(input, output, sizeof(output));
#ifdef CONFIG_AI_AGENT_LABTWIN_ENV_TEST
        } else if (!strcmp(argv[2], "inject") && argc >= 4 &&
                   !strcmp(argv[3], "clear")) {
            cJSON_Delete(root); root = NULL;
            ret = labtwin_environment_inject(NULL, 0, true);
            snprintf(output, sizeof(output),
                     "{\"ok\":%s,\"code\":\"%s\"}",
                     ret == 0 ? "true" : "false", ret == 0 ? "OK" : "INVALID_ARGUMENT");
        } else if (!strcmp(argv[2], "inject") && argc >= 5) {
            cJSON_Delete(root); root = NULL;
            ret = labtwin_environment_inject(argv[3], strtof(argv[4], NULL), false);
            snprintf(output, sizeof(output),
                     "{\"ok\":%s,\"code\":\"%s\"}",
                     ret == 0 ? "true" : "false", ret == 0 ? "OK" : "INVALID_ARGUMENT");
#endif
        } else {
            cJSON_Delete(root); print_usage(); return;
        }
    } else if (!strcmp(argv[1], "sensor")) {
        cJSON_Delete(root); root = NULL;
        ret = labtwin_sensor_snapshot_json(output, sizeof(output));
    } else {
        cJSON_Delete(root); print_usage(); return;
    }
    free(input);
    printf("%s\n", output);
    (void)ret;
}
