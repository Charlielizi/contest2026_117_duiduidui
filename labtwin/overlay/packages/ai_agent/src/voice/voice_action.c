#include "voice/voice_action.h"
#include "voice/voice_request_ledger.h"
#include "tools/tool_registry.h"
#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static voice_action_snapshot_t g_action;

static int has_string(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring && item->valuestring[0];
}

static int has_optional_string(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return !item || (cJSON_IsString(item) && item->valuestring &&
                     item->valuestring[0]);
}

static int number_in_range(cJSON *root, const char *name,
                           double minimum, double maximum)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) && item->valuedouble >= minimum &&
           item->valuedouble <= maximum &&
           item->valuedouble == (double)item->valueint;
}

static int valid_steps(cJSON *steps)
{
    cJSON *item;
    int count;

    if (!cJSON_IsArray(steps)) return 0;
    count = cJSON_GetArraySize(steps);
    if (count < 1 || count > 16) return 0;
    cJSON_ArrayForEach(item, steps) {
        if (cJSON_IsString(item) && item->valuestring &&
            item->valuestring[0]) continue;
        if (cJSON_IsObject(item) && has_string(item, "title")) continue;
        return 0;
    }
    return 1;
}

static int valid_transition(cJSON *root)
{
    static const char *const actions[] = {
        "start", "pause", "resume", "complete_step", "complete", "cancel"
    };
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "action");
    size_t i;

    if (!cJSON_IsString(item) || !item->valuestring) return 0;
    for (i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (strcmp(item->valuestring, actions[i]) == 0) return 1;
    return 0;
}

static int validate_candidate(const char *tool, const char *arguments)
{
    cJSON *root = cJSON_Parse(arguments);
    int valid = 0;

    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -EINVAL;
    }
    if (strcmp(tool, "experiment_create") == 0) {
        valid = has_optional_string(root, "experiment_id") &&
                has_string(root, "name") &&
                valid_steps(cJSON_GetObjectItemCaseSensitive(root, "steps"));
    } else if (strcmp(tool, "experiment_transition") == 0) {
        valid = has_string(root, "experiment_id") && valid_transition(root);
    } else if (strcmp(tool, "timer_start") == 0) {
        valid = has_string(root, "experiment_id") &&
                number_in_range(root, "step_index", 0, 15) &&
                has_string(root, "label") &&
                number_in_range(root, "duration_seconds", 1, 604800);
    } else if (strcmp(tool, "timer_cancel") == 0) {
        valid = has_string(root, "experiment_id") &&
                has_string(root, "timer_id");
    } else if (strcmp(tool, "lab_log_add") == 0) {
        cJSON *structured = cJSON_GetObjectItemCaseSensitive(root, "structured");
        valid = has_string(root, "experiment_id") && has_string(root, "text") &&
                (!structured || cJSON_IsObject(structured));
    } else if (strcmp(tool, "environment_event_ack") == 0) {
        valid = has_string(root, "event_id");
    } else {
        /* High-risk non-LabTwin tools still use the common confirmation
         * pipeline.  Their registry schemas and tool guards perform the
         * tool-specific validation; require at least a JSON object here. */
        valid = 1;
    }
    cJSON_Delete(root);
    return valid ? 0 : -EINVAL;
}

static const char *transition_summary(const char *action)
{
    if (!action) return "变更实验状态";
    if (strcmp(action, "start") == 0) return "开始实验";
    if (strcmp(action, "pause") == 0) return "暂停实验";
    if (strcmp(action, "resume") == 0) return "恢复实验";
    if (strcmp(action, "complete_step") == 0) return "完成当前步骤";
    if (strcmp(action, "complete") == 0) return "结束实验";
    if (strcmp(action, "cancel") == 0) return "取消实验";
    return "变更实验状态";
}

static void describe_candidate(const char *tool, const char *arguments,
                               char *summary, size_t summary_size)
{
    cJSON *root = cJSON_Parse(arguments);
    const char *experiment_id = NULL;
    const char *name = NULL;

    if (!summary || summary_size == 0) {
        cJSON_Delete(root);
        return;
    }
    summary[0] = 0;
    if (!root) return;
    experiment_id = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "experiment_id"));
    name = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "name"));

    if (strcmp(tool, "experiment_create") == 0) {
        cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
        snprintf(summary, summary_size, "创建实验“%s”（%d 个步骤）",
                 name ? name : "未命名", cJSON_GetArraySize(steps));
    } else if (strcmp(tool, "experiment_transition") == 0) {
        const char *action = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "action"));
        snprintf(summary, summary_size, "%s：%s",
                 transition_summary(action), experiment_id ? experiment_id : "");
    } else if (strcmp(tool, "timer_start") == 0) {
        const char *label = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "label"));
        cJSON *step = cJSON_GetObjectItemCaseSensitive(root, "step_index");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(root,
                                                           "duration_seconds");
        snprintf(summary, summary_size, "实验 %s 第 %d 步启动“%s”计时 %d 秒",
                 experiment_id ? experiment_id : "", (int)step->valueint + 1,
                 label ? label : "", (int)duration->valueint);
    } else if (strcmp(tool, "timer_cancel") == 0) {
        const char *timer_id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "timer_id"));
        snprintf(summary, summary_size, "取消实验 %s 的计时器 %s",
                 experiment_id ? experiment_id : "", timer_id ? timer_id : "");
    } else if (strcmp(tool, "lab_log_add") == 0) {
        snprintf(summary, summary_size, "向实验 %s 追加观察记录",
                 experiment_id ? experiment_id : "");
    } else if (strcmp(tool, "environment_event_ack") == 0) {
        const char *event_id = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(root, "event_id"));
        snprintf(summary, summary_size, "确认环境告警 %s",
                 event_id ? event_id : "");
    } else {
        snprintf(summary, summary_size, "执行 %s", tool);
    }
    cJSON_Delete(root);
}

int voice_action_is_mutating_tool(const char *tool)
{
    return tool_registry_requires_confirmation(tool);
}

int voice_action_prepare(const char *request_id, const char *transcript,
                         const char *tool, const char *arguments,
                         char *output, size_t output_size)
{
    char summary[sizeof(g_action.summary)];

    if (!request_id || !request_id[0] || !transcript || !tool ||
        !voice_action_is_mutating_tool(tool) || !arguments || !output ||
        output_size == 0)
        return -EINVAL;
    if (validate_candidate(tool, arguments) != 0) return -EINVAL;
    if (strlen(request_id) >= sizeof(g_action.request_id) ||
        strlen(transcript) >= sizeof(g_action.transcript) ||
        strlen(tool) >= sizeof(g_action.tool) ||
        strlen(arguments) >= sizeof(g_action.arguments))
        return -E2BIG;
    describe_candidate(tool, arguments, summary, sizeof(summary));

    pthread_mutex_lock(&g_lock);
    if (g_action.state == VOICE_ACTION_EXECUTING) {
        pthread_mutex_unlock(&g_lock);
        return -EBUSY;
    }
    if (g_action.state == VOICE_ACTION_PENDING) {
        if (strcmp(g_action.request_id, request_id) == 0 &&
            strcmp(g_action.tool, tool) == 0 &&
            strcmp(g_action.arguments, arguments) == 0) {
            snprintf(output, output_size,
                "{\"ok\":true,\"code\":\"PENDING_CONFIRMATION\","
                "\"request_id\":\"%s\",\"tool\":\"%s\"}",
                g_action.request_id, g_action.tool);
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        pthread_mutex_unlock(&g_lock);
        return -EBUSY;
    }

    voice_ledger_state_t persisted_state = VOICE_LEDGER_PENDING;
    char persisted_result[sizeof(g_action.result)] = {0};
    int ledger_ret = voice_request_ledger_prepare(request_id, tool, arguments,
                                                  transcript);
    if (ledger_ret < 0) {
        pthread_mutex_unlock(&g_lock);
        return ledger_ret;
    }
    if (ledger_ret == 1 &&
        voice_request_ledger_lookup(request_id, &persisted_state,
                                    persisted_result,
                                    sizeof(persisted_result)) == 0 &&
        persisted_state != VOICE_LEDGER_PENDING) {
        memset(&g_action, 0, sizeof(g_action));
        snprintf(g_action.request_id, sizeof(g_action.request_id), "%s",
                 request_id);
        snprintf(g_action.tool, sizeof(g_action.tool), "%s", tool);
        snprintf(g_action.arguments, sizeof(g_action.arguments), "%s",
                 arguments);
        snprintf(g_action.transcript, sizeof(g_action.transcript), "%s",
                 transcript);
        snprintf(g_action.summary, sizeof(g_action.summary), "%s", summary);
        if (persisted_state == VOICE_LEDGER_EXECUTING) {
            g_action.state = VOICE_ACTION_FAILED;
            snprintf(g_action.result, sizeof(g_action.result),
                "{\"ok\":false,\"code\":\"ACTION_OUTCOME_UNKNOWN\","
                "\"message\":\"The previous execution was interrupted; "
                "inspect device state before retrying.\"}");
        } else {
            g_action.state = persisted_state == VOICE_LEDGER_APPLIED ?
                VOICE_ACTION_APPLIED :
                (persisted_state == VOICE_LEDGER_CANCELLED ?
                 VOICE_ACTION_CANCELLED : VOICE_ACTION_FAILED);
            snprintf(g_action.result, sizeof(g_action.result), "%s",
                     persisted_result);
        }
        snprintf(output, output_size, "%s", g_action.result);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    memset(&g_action, 0, sizeof(g_action));
    g_action.state = VOICE_ACTION_PENDING;
    snprintf(g_action.request_id, sizeof(g_action.request_id), "%s", request_id);
    snprintf(g_action.transcript, sizeof(g_action.transcript), "%s", transcript);
    snprintf(g_action.tool, sizeof(g_action.tool), "%s", tool);
    snprintf(g_action.arguments, sizeof(g_action.arguments), "%s", arguments);
    snprintf(g_action.summary, sizeof(g_action.summary), "%s", summary);
    snprintf(output, output_size,
        "{\"ok\":true,\"code\":\"PENDING_CONFIRMATION\","
        "\"request_id\":\"%s\",\"tool\":\"%s\","
        "\"message\":\"Show the transcript and action on screen; do not say it was applied.\"}",
        g_action.request_id, g_action.tool);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int voice_action_confirm(char *output, size_t output_size)
{
    char tool[40];
    char arguments[1024];
    char request_id[40];
    if (!output || output_size == 0) return -EINVAL;

    pthread_mutex_lock(&g_lock);
    if (g_action.state == VOICE_ACTION_APPLIED) {
        snprintf(output, output_size, "%s", g_action.result);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    if (g_action.state == VOICE_ACTION_EXECUTING) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"code\":\"ACTION_IN_PROGRESS\"}");
        pthread_mutex_unlock(&g_lock);
        return -EBUSY;
    }
    if (g_action.state != VOICE_ACTION_PENDING) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }
    snprintf(tool, sizeof(tool), "%s", g_action.tool);
    snprintf(arguments, sizeof(arguments), "%s", g_action.arguments);
    snprintf(request_id, sizeof(request_id), "%s", g_action.request_id);
    g_action.state = VOICE_ACTION_EXECUTING;
    pthread_mutex_unlock(&g_lock);

    voice_ledger_state_t persisted_state;
    char persisted_result[1024] = {0};
    if (voice_request_ledger_lookup(request_id, &persisted_state,
                                    persisted_result,
                                    sizeof(persisted_result)) == 0 &&
        persisted_state == VOICE_LEDGER_APPLIED) {
        pthread_mutex_lock(&g_lock);
        g_action.state = VOICE_ACTION_APPLIED;
        snprintf(g_action.result, sizeof(g_action.result), "%s",
                 persisted_result);
        snprintf(output, output_size, "%s", persisted_result);
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    int persist_ret = voice_request_ledger_finish(request_id,
        VOICE_LEDGER_EXECUTING,
        "{\"ok\":false,\"code\":\"ACTION_IN_PROGRESS\"}");
    if (persist_ret != 0) {
        pthread_mutex_lock(&g_lock);
        g_action.state = VOICE_ACTION_PENDING;
        snprintf(output, output_size,
            "{\"ok\":false,\"code\":\"LEDGER_UNAVAILABLE\",\"error\":%d}",
            persist_ret);
        pthread_mutex_unlock(&g_lock);
        return persist_ret;
    }

    char result[1024] = {0};
    int tool_ret = tool_registry_execute(tool, arguments, result, sizeof(result));
    if (!result[0]) {
        snprintf(result, sizeof(result),
                 "{\"ok\":false,\"code\":\"TOOL_EXECUTION_FAILED\","
                 "\"error\":%d}", tool_ret);
    }
    persist_ret = voice_request_ledger_finish(request_id,
        tool_ret == 0 ? VOICE_LEDGER_APPLIED : VOICE_LEDGER_FAILED, result);
    int ret = tool_ret;
    if (persist_ret != 0) {
        ret = persist_ret;
        snprintf(result, sizeof(result),
            "{\"ok\":false,\"code\":\"ACTION_OUTCOME_UNKNOWN\","
            "\"message\":\"The tool returned but its durable result was not saved.\","
            "\"error\":%d}", persist_ret);
    }

    pthread_mutex_lock(&g_lock);
    g_action.state = tool_ret == 0 && persist_ret == 0 ?
        VOICE_ACTION_APPLIED : VOICE_ACTION_FAILED;
    snprintf(g_action.result, sizeof(g_action.result), "%s", result);
    snprintf(output, output_size, "%s", result);
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int voice_action_cancel(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_action.state != VOICE_ACTION_PENDING) {
        pthread_mutex_unlock(&g_lock);
        return -ENOENT;
    }
    int ret = voice_request_ledger_finish(g_action.request_id,
                                          VOICE_LEDGER_CANCELLED,
                                          "{\"ok\":false,\"code\":\"CANCELLED\"}");
    if (ret != 0) {
        pthread_mutex_unlock(&g_lock);
        return ret;
    }
    g_action.state = VOICE_ACTION_CANCELLED;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void voice_action_get_snapshot(voice_action_snapshot_t *snapshot)
{
    if (!snapshot) return;
    pthread_mutex_lock(&g_lock);
    *snapshot = g_action;
    pthread_mutex_unlock(&g_lock);
}
