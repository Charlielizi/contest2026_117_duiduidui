/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "tools/tool_registry.h"
#ifdef CONFIG_AI_AGENT_MCP
#include "tools/mcp_client.h"
#include "tools/mcp_bridge.h"
#endif
#include "tools/tool_guard.h"
#include "tools/tool_control.h"
#include "tools/tool_board_output.h"
#include "tools/tool_cron.h"
#include "tools/tool_feishu_chat.h"
#include "tools/tool_feishu_doc.h"
#include "tools/tool_fetch_url.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_health.h"
#include "tools/tool_labtwin.h"
#include "tools/tool_media.h"
#include "tools/tool_memory.h"
#include "tools/tool_proxyquickapp.h"
#include "tools/tool_shell.h"
#include "tools/tool_system.h"
#include "tools/tool_vision.h"
#include "tools/tool_camera.h"
#include "tools/tool_web_search.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include <pthread.h>
#include <string.h>

static const char* TAG = "tools";

#define MAX_TOOLS 58
#define MAX_PROVIDERS 4

/* ── External tool provider registry ───────────────────────── */

typedef struct {
    const char* name;
    tool_provider_fn get_tools;
    tool_executor_fn execute;
} tool_provider_t;

static tool_provider_t s_providers[MAX_PROVIDERS];
static int s_provider_count = 0;

static agent_tool_t s_tools[MAX_TOOLS];
static int s_tool_count;
static char* s_tools_json;
static bool s_tools_dirty = true;
static pthread_mutex_t s_tools_mtx = PTHREAD_MUTEX_INITIALIZER;

void tool_registry_register_provider(const char* name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute)
{
    if (s_provider_count >= MAX_PROVIDERS) {
        syslog(LOG_ERR, "[%s] Provider registry full, cannot add '%s'\n",
               TAG, name ? name : "(null)");
        return;
    }
    s_providers[s_provider_count].name = name;
    s_providers[s_provider_count].get_tools = get_tools;
    s_providers[s_provider_count].execute = execute;
    s_provider_count++;
    syslog(LOG_INFO, "[%s] Registered tool provider: %s\n", TAG, name);
}

static void register_tool(const agent_tool_t* tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        syslog(LOG_ERR, "[%s] Tool registry full\n", TAG);
        return;
    }
    s_tools[s_tool_count++] = *tool;
    syslog(LOG_INFO, "[%s] Registered tool: %s\n", TAG, tool->name);
}

/* Rebuild tools JSON under lock. Only rebuilds when dirty flag is set. */
static void build_tools_json_locked(void)
{
    if (!s_tools_dirty) {
        return;
    }

    cJSON* arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);
        cJSON* schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema)
            cJSON_AddItemToObject(tool, "input_schema", schema);
        cJSON_AddItemToArray(arr, tool);
    }

#ifdef CONFIG_AI_AGENT_MCP
    char* mcp_json = mcp_bridge_get_tools_json();
    if (mcp_json) {
        cJSON* mcp_arr = cJSON_Parse(mcp_json);
        if (mcp_arr && cJSON_IsArray(mcp_arr)) {
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, mcp_arr)
            {
                cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
            }
        }
        cJSON_Delete(mcp_arr);
        free(mcp_json);
    }
#endif

    /* Collect tools from all registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].get_tools) {
            continue;
        }
        char* provider_json = s_providers[p].get_tools();
        if (provider_json) {
            cJSON* provider_arr = cJSON_Parse(provider_json);
            if (provider_arr && cJSON_IsArray(provider_arr)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, provider_arr)
                {
                    cJSON_AddItemToArray(arr, cJSON_Duplicate(item, 1));
                }
            }
            cJSON_Delete(provider_arr);
            free(provider_json);
        }
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    s_tools_dirty = false;
    cJSON_Delete(arr);
    syslog(LOG_INFO, "[%s] Tools JSON built (%d builtin + %d providers)\n",
        TAG, s_tool_count, s_provider_count);
}

void tool_registry_rebuild_json(void) { build_tools_json_locked(); }

int tool_registry_init(void)
{
    s_tool_count = 0;

#ifdef CONFIG_AI_AGENT_MCP
    if (mcp_bridge_init() != OK) {
        syslog(LOG_WARNING, "[%s] MCP bridge init failed; MCP tools disabled\n",
            TAG);
    }

    if (mcp_client_init() != OK) {
        syslog(LOG_WARNING, "[%s] MCP client init failed; remote tools disabled\n",
            TAG);
    }
#endif

    tool_web_search_init();

    REGISTER_TOOL_CONFIRM("experiment_create",
        "Propose a local laboratory experiment from an explicit name and step list. Preserve an optional description and user-supplied schedule as Unix epoch seconds. experiment_id is optional; omit it to let the device generate a unique ID.",
        "{\"type\":\"object\",\"properties\":{\"experiment_id\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\",\"maxLength\":512},\"planned_start_epoch\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Planned start time as Unix epoch seconds\"},\"planned_end_epoch\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Planned end time as Unix epoch seconds\"},\"steps\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16,\"items\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\"}},\"required\":[\"title\"],\"additionalProperties\":false}]}}},\"required\":[\"name\",\"steps\"],\"additionalProperties\":false}",
        tool_experiment_create_execute);
    REGISTER_TOOL("experiment_get", "Get authoritative state, current step, and timers for one experiment ID.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Experiment ID") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\""),
        tool_experiment_get_execute);
    REGISTER_TOOL("experiment_list", "List local experiments. Use this to resolve 'current experiment' before a write when the user did not provide an ID.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("state", "Optional state filter") TOOL_SCHEMA_END(),
        tool_experiment_list_execute);
    REGISTER_TOOL_CONFIRM("experiment_transition", "Propose one validated state transition for an existing experiment. Query first if the target or current state is uncertain.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Experiment ID") "," TOOL_PARAM_ENUM("action", "Transition", "\"start\",\"pause\",\"resume\",\"complete_step\",\"complete\",\"cancel\"") "," TOOL_PARAM_STR("confirmation", "Optional confirmation note") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\",\"action\""),
        tool_experiment_transition_execute);
    REGISTER_TOOL_CONFIRM("experiment_delete", "Request administrator-confirmed deletion. Only the portal confirmation endpoint can execute this action.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Exact experiment ID") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\""),
        tool_experiment_delete_execute);
    REGISTER_TOOL_CONFIRM("experiment_update", "Edit only a READY experiment. Query first and provide its last_event_seq as if_event_seq.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Experiment ID") "," TOOL_PARAM_STR("name", "Name") "," TOOL_PARAM_STR("description", "Description") "," TOOL_PARAM_NUM("planned_start_epoch", "Optional Unix seconds") "," TOOL_PARAM_NUM("planned_end_epoch", "Optional Unix seconds") "," "\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}" "," TOOL_PARAM_NUM("if_event_seq", "Queried last_event_seq") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\",\"name\",\"steps\",\"if_event_seq\""),
        tool_experiment_update_execute);
    REGISTER_TOOL_CONFIRM("timer_start", "Start an experiment-step timer.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Experiment ID") "," TOOL_PARAM_NUM("step_index", "Zero-based step index") "," TOOL_PARAM_STR("label", "Timer label") "," TOOL_PARAM_NUM("duration_seconds", "Duration from 1 to 604800 seconds") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\",\"step_index\",\"label\",\"duration_seconds\""),
        tool_timer_start_execute);
    REGISTER_TOOL_CONFIRM("timer_cancel", "Cancel an active experiment timer.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("experiment_id", "Experiment ID") "," TOOL_PARAM_STR("timer_id", "Timer ID") "," TOOL_PARAM_STR("reason", "Optional reason") TOOL_SCHEMA_END_REQUIRED("\"experiment_id\",\"timer_id\""),
        tool_timer_cancel_execute);
    REGISTER_TOOL_CONFIRM("lab_log_add", "Append a factual observation to an experiment.",
        "{\"type\":\"object\",\"properties\":{\"experiment_id\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"},\"structured\":{\"type\":\"object\"}},\"required\":[\"experiment_id\",\"text\"],\"additionalProperties\":false}",
        tool_lab_log_add_execute);
    REGISTER_TOOL_NO_PARAMS("sensor_snapshot", "Read cached local laboratory sensors.",
        tool_sensor_snapshot_execute);
    REGISTER_TOOL("environment_event_list",
        "List authoritative local environment warning events.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_ENUM("state", "Event filter", "\"active\",\"all\"") TOOL_SCHEMA_END(),
        tool_environment_event_list_execute);
    REGISTER_TOOL_CONFIRM("environment_event_ack",
        "Propose acknowledgement of an active local environment warning. This records acknowledgement only and never clears or certifies the physical condition.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("event_id", "Environment event ID") TOOL_SCHEMA_END_REQUIRED("\"event_id\""),
        tool_environment_event_ack_execute);

    /* Protocol / SOP tools */
    REGISTER_TOOL("protocol_validate",
        "Validate a structured SOP/experiment protocol JSON document against the LabTwin protocol schema. Returns schema errors or OK. Use this before protocol_create to catch issues early.",
        "{\"type\":\"object\",\"properties\":{\"schema_version\":{\"type\":\"integer\"},\"protocol_id\":{\"type\":\"string\"},\"version\":{\"type\":\"integer\",\"minimum\":1},\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"parameters\":{\"type\":\"array\"},\"prerequisites\":{\"type\":\"array\"},\"steps\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16},\"acceptance_criteria\":{\"type\":\"array\"}},\"required\":[\"protocol_id\",\"version\",\"name\",\"parameters\",\"prerequisites\",\"steps\",\"acceptance_criteria\"]}",
        tool_protocol_validate_execute);
    REGISTER_TOOL_CONFIRM("protocol_create",
        "Create a new immutable SOP/experiment protocol version. The protocol is validated and persisted; existing versions cannot be overwritten. Version must start at 1 and increase by 1.",
        "{\"type\":\"object\",\"properties\":{\"schema_version\":{\"type\":\"integer\"},\"protocol_id\":{\"type\":\"string\"},\"version\":{\"type\":\"integer\",\"minimum\":1},\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"parameters\":{\"type\":\"array\"},\"prerequisites\":{\"type\":\"array\"},\"steps\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":16},\"acceptance_criteria\":{\"type\":\"array\"}},\"required\":[\"protocol_id\",\"version\",\"name\",\"parameters\",\"prerequisites\",\"steps\",\"acceptance_criteria\"]}",
        tool_protocol_create_execute);
    REGISTER_TOOL("protocol_get",
        "Read one immutable protocol version by ID and version. Returns the full protocol JSON.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("protocol_id", "Protocol ID") "," TOOL_PARAM_NUM("version", "Protocol version number") TOOL_SCHEMA_END_REQUIRED("\"protocol_id\",\"version\""),
        tool_protocol_get_execute);
    REGISTER_TOOL("protocol_list",
        "List stored SOP/experiment protocol summaries. Optionally filter by ID to see all versions of one protocol.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("protocol_id", "Optional protocol ID filter") "," TOOL_PARAM_NUM("offset", "Optional result offset") "," TOOL_PARAM_NUM("limit", "Optional result limit") TOOL_SCHEMA_END(),
        tool_protocol_list_execute);

    /* Closed-loop controller tools */
    REGISTER_TOOL_CONFIRM("controller_create",
        "Create a closed-loop experiment run bound to one immutable protocol version and a set of parameters. The run starts in PLAN phase. Optionally link to a READY experiment with matching steps; one is created from the protocol if absent.",
        "{\"type\":\"object\",\"properties\":{\"run_id\":{\"type\":\"string\"},\"protocol_id\":{\"type\":\"string\"},\"protocol_version\":{\"type\":\"integer\",\"minimum\":1},\"experiment_id\":{\"type\":\"string\"},\"parameters\":{\"type\":\"object\"}},\"required\":[\"run_id\",\"protocol_id\",\"protocol_version\",\"parameters\"]}",
        tool_controller_create_execute);
    REGISTER_TOOL("controller_get",
        "Get the current state of a controller run, including phase, current step, sequence number, and reason.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("run_id", "Controller run ID") TOOL_SCHEMA_END_REQUIRED("\"run_id\""),
        tool_controller_get_execute);
    REGISTER_TOOL_CONFIRM("controller_command",
        "Apply one guarded state-machine action to a controller run. Always include expected_seq from the latest status to prevent race conditions. Actions: plan, execute, observe, evaluate, pause, resume, request_help, abort, timeout, failure.",
        "{\"type\":\"object\",\"properties\":{\"run_id\":{\"type\":\"string\"},\"expected_seq\":{\"type\":\"integer\",\"minimum\":1},\"action\":{\"type\":\"string\",\"enum\":[\"plan\",\"execute\",\"observe\",\"evaluate\",\"pause\",\"resume\",\"request_help\",\"abort\",\"timeout\",\"failure\"]},\"evidence\":{\"type\":\"object\",\"description\":\"Observation data for evaluate/execute actions\"},\"reason\":{\"type\":\"string\"}},\"required\":[\"run_id\",\"expected_seq\",\"action\"]}",
        tool_controller_command_execute);

    /* Confirmed long-term memory.  The agent loop injects the caller
     * identity, so a model cannot write a proposal for another user. */
    REGISTER_TOOL("memory_propose",
        "Propose one stable laboratory context or personal preference for later confirmation. This creates only a 24-hour draft. Ask the user to reply 确认保存 or /memory confirm; never claim it was saved before confirmation.",
        "{\"type\":\"object\",\"properties\":{\"scope\":{\"type\":\"string\",\"enum\":[\"lab\",\"user\"]},\"kind\":{\"type\":\"string\"},\"content\":{\"type\":\"string\",\"maxLength\":256},\"keywords\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":6,\"items\":{\"type\":\"string\",\"maxLength\":48}}},\"required\":[\"scope\",\"kind\",\"content\",\"keywords\"],\"additionalProperties\":false}",
        tool_memory_propose_execute);

    /* Immutable, operator-confirmed experiment report cards. */
    REGISTER_TOOL_CONFIRM("report_create",
        "Create one immutable local experiment report card from user-provided summary and numeric performance metrics. Link experiment_id only when known. Corrections create another card using supersedes_report_id; never overwrite a report.",
        "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"maxLength\":96},\"summary\":{\"type\":\"string\",\"maxLength\":512},\"tags\":{\"type\":\"array\",\"maxItems\":8,\"items\":{\"type\":\"string\",\"maxLength\":32}},\"metrics\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":12,\"items\":{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"value\":{\"type\":\"number\"},\"unit\":{\"type\":\"string\"},\"condition\":{\"type\":\"string\"}},\"required\":[\"key\",\"value\",\"unit\",\"condition\"],\"additionalProperties\":false}},\"experiment_id\":{\"type\":\"string\"},\"supersedes_report_id\":{\"type\":\"string\"}},\"required\":[\"title\",\"summary\",\"tags\",\"metrics\"],\"additionalProperties\":false}",
        tool_report_create_execute);
    REGISTER_TOOL("report_search",
        "Search confirmed local report cards by words, linked experiment, metric name, numeric range, time range, or sort order. Cite the returned report_id, metric unit, and condition before making a historical-performance claim.",
        "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"experiment_id\":{\"type\":\"string\"},\"metric_key\":{\"type\":\"string\"},\"min_value\":{\"type\":\"number\"},\"max_value\":{\"type\":\"number\"},\"from_epoch\":{\"type\":\"number\"},\"to_epoch\":{\"type\":\"number\"},\"sort\":{\"type\":\"string\",\"enum\":[\"newest\",\"metric_asc\",\"metric_desc\"]},\"limit\":{\"type\":\"number\",\"minimum\":1,\"maximum\":8}},\"additionalProperties\":false}",
        tool_report_search_execute);
    REGISTER_TOOL("report_get",
        "Read one full immutable report card by report_id, including its linked experiment, conclusion, metrics, units, and conditions.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("report_id", "Report card ID") TOOL_SCHEMA_END_REQUIRED("\"report_id\""),
        tool_report_get_execute);

    /* Web / info tools */
    REGISTER_TOOL(
        "web_search",
        "Search the web for current info, news, or real-time data.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("query", "The search query")
            TOOL_SCHEMA_END_REQUIRED("\"query\""),
        tool_web_search_execute);

    REGISTER_TOOL(
        "news_search",
        "Search recent news. Set top_headlines=true for major stories.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("query", "News search query") "," TOOL_PARAM_BOOL(
                "top_headlines",
                "If true, fetch top headlines instead of everything")
                TOOL_SCHEMA_END_REQUIRED("\"query\""),
        tool_news_search_execute);

    REGISTER_TOOL(
        "get_weather",
        "Get current weather for a location.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "location", "City name or location, e.g. 'Shanghai' or 'New York'")
            TOOL_SCHEMA_END_REQUIRED("\"location\""),
        tool_get_weather_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_current_time",
        "Get current date, time, and UNIX epoch.",
        tool_get_time_execute);

    /* File tools */
    REGISTER_TOOL("read_file",
        "Read a file. Path must start with " AGENT_DATA_DIR "/.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("path", "Absolute file path")
            TOOL_SCHEMA_END_REQUIRED("\"path\""),
        tool_read_file_execute);

    REGISTER_TOOL_CONFIRM(
        "write_file",
        "Write a file. Path must start with " AGENT_DATA_DIR "/.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("path", "Absolute file path") "," TOOL_PARAM_STR(
                "content", "File content")
                TOOL_SCHEMA_END_REQUIRED("\"path\",\"content\""),
        tool_write_file_execute);

    REGISTER_TOOL_CONFIRM(
        "edit_file", "Find and replace text in a file.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("path", "Absolute file path") "," TOOL_PARAM_STR(
                "old_string",
                "Text to find") "," TOOL_PARAM_STR("new_string",
                "Replacement text")
                TOOL_SCHEMA_END_REQUIRED(
                    "\"path\",\"old_string\",\"new_string\""),
        tool_edit_file_execute);

    REGISTER_TOOL(
        "list_dir",
        "List files in data directory.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("prefix", "Optional path prefix")
            TOOL_SCHEMA_END(),
        tool_list_dir_execute);

    /* Cron tools */
    REGISTER_TOOL_CONFIRM(
        "cron_add",
        "Schedule a reminder or task. "
        "Steps: get_current_time→compute at_epoch→call this. "
        "at_epoch must be plain integer. "
        "Recurring: schedule_type='every', interval_s=seconds. "
        "Action: set action=tool_name, action_args=JSON.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("name", "Job name") "," TOOL_PARAM_ENUM("schedule_type", "Schedule type", "\"every\",\"at\"") "," TOOL_PARAM_NUM("interval_s", "Interval in seconds (for every)") "," TOOL_PARAM_NUM("at_epoch",
                "Target UNIX timestamp as a plain integer") "," TOOL_PARAM_STR("message",
                "Notification message sent to user when job fires") "," TOOL_PARAM_STR("channel", "Reply channel (default system)") "," TOOL_PARAM_STR("chat_id", "Reply chat_id") "," TOOL_PARAM_STR("action",
                "Tool name to execute when job fires (e.g. music_play)") "," TOOL_PARAM_STR("action_args",
                "JSON arguments for the action tool")
                TOOL_SCHEMA_END_REQUIRED("\"name\",\"schedule_type\",\"message\""),
        tool_cron_add_execute);

    REGISTER_TOOL_NO_PARAMS("cron_list", "List all scheduled cron jobs.",
        tool_cron_list_execute);

    REGISTER_TOOL_CONFIRM("cron_remove", "Remove a scheduled cron job by its ID.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("job_id", "8-char hex job ID")
                TOOL_SCHEMA_END_REQUIRED("\"job_id\""),
        tool_cron_remove_execute);

    /* Fetch URL tool */
    REGISTER_TOOL(
        "fetch_url",
        "Fetch content from an HTTPS URL.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("url", "The HTTPS URL to fetch")
            TOOL_SCHEMA_END_REQUIRED("\"url\""),
        tool_fetch_url_execute);

    /* Vision tool */
    REGISTER_TOOL_CONFIRM(
        "analyze_image",
        "Analyze a screen screenshot or image file with vision LLM. "
        "Without image_path: auto-captures screen. With image_path: reads file.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("image_path", "Optional: path to an existing image "
                                         "file. Omit to auto-capture screen.") "," TOOL_PARAM_STR("prompt", "Question or instruction about the image")
                TOOL_SCHEMA_END(),
        tool_analyze_image_execute);

    /* Camera capture tool */
#ifdef CONFIG_AI_AGENT_CAMERA
    REGISTER_TOOL_CONFIRM(
        "camera_capture",
        "Take a photo with device camera and analyze it. "
        "For screen screenshots, use analyze_image instead.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("prompt",
                "Optional question about what the camera sees") ","
            TOOL_PARAM_ENUM("resolution",
                "Image resolution: low (320x180, faster) or high (1280x720)",
                "\"low\",\"high\"")
            TOOL_SCHEMA_END(),
        tool_camera_capture_execute);
#endif

    /* Shell tool */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_FULL
    REGISTER_TOOL_CONFIRM(
        "run_shell",
        "Execute a NuttX NSH command. FULL mode: pipes/redirects allowed. "
        "NuttX, not Linux — no Linux flags (use 'free' not 'free -h'). "
        "lvctl: 'lvctl snapshot take 1', 'lvctl snapshot save /data/screen.png'.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("command", "The NSH command to execute, e.g. 'free' "
                                      "or 'curl wttr.in/Beijing | head -7'")
                TOOL_SCHEMA_END_REQUIRED("\"command\""),
        tool_run_shell_execute);
#elif AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_DENY
    /* Shell tool disabled in DENY mode */
#else
    REGISTER_TOOL_CONFIRM(
        "run_shell",
        "Execute a NuttX NSH command. ALLOWLIST mode: only safe commands. "
        "NuttX, not Linux — no Linux flags. No pipes/redirects. "
        "Allowed: free, ps, df, cat, ls, curl, lvctl, nxplayer, etc.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "command", "The NSH command to execute, e.g. 'free' or 'echo hello'")
            TOOL_SCHEMA_END_REQUIRED("\"command\""),
        tool_run_shell_execute);
#endif

    /* System state tools */
    REGISTER_TOOL_NO_PARAMS(
        "get_battery",
        "Get battery level, charging status, voltage, temperature.",
        tool_get_battery_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_wear_state",
        "Check if device is being worn.",
        tool_get_wear_state_execute);

    REGISTER_TOOL_NO_PARAMS(
        "get_screen_state",
        "Check if screen is on or off.",
        tool_get_screen_state_execute);

    /* Health / fitness tools */
    REGISTER_TOOL_NO_PARAMS(
        "get_heartrate",
        "Get latest heart rate in BPM.",
        tool_get_heartrate_execute);

    REGISTER_TOOL_NO_PARAMS("get_steps",
        "Get step count and step frequency.",
        tool_get_steps_execute);

    /* Device control tools */
    REGISTER_TOOL(
        "vibrate",
        "Trigger vibration. duration_ms: 1-5000 (default 200), amplitude: 1-255.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_NUM(
            "duration_ms",
            "Duration in ms (1-5000)") "," TOOL_PARAM_NUM("amplitude",
            "Strength 1-255")
            TOOL_SCHEMA_END(),
        tool_vibrate_execute);

    /* Board feedback tools.  Voice-originated calls retain local confirmation;
     * authenticated web requests execute only the bounded, non-persistent
     * board feedback requested by the user. */
    REGISTER_TOOL_CONFIRM(
        "board_display",
        "Show short text on the physical board screen. Text is limited to 180 bytes and the overlay auto-closes in 1-30 seconds.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("text", "Text to show on the board") "," TOOL_PARAM_NUM("duration_seconds", "Visible seconds, 1-30; default 10") TOOL_SCHEMA_END_REQUIRED("\"text\""),
        tool_board_display_execute);
    REGISTER_TOOL_CONFIRM(
        "board_speak",
        "Speak short text through the physical board speaker. Text is limited to 160 bytes; only one board speech request may run at once. Report it as queued, not completed.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("text", "Text to speak on the board") TOOL_SCHEMA_END_REQUIRED("\"text\""),
        tool_board_speak_execute);

    /* Feishu document tools */
    REGISTER_TOOL_CONFIRM(
        "feishu_doc_create",
        "Create a new Feishu cloud document. Returns document_id and URL.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("title", "Document title") "," TOOL_PARAM_STR(
                "folder_token", "Target folder token (optional, has default)")
                TOOL_SCHEMA_END_REQUIRED("\"title\""),
        tool_feishu_doc_create_execute);

    REGISTER_TOOL_CONFIRM(
        "feishu_doc_write",
        "Write text into a Feishu document. Each line becomes a paragraph.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR(
            "document_id",
            "Feishu document ID") "," TOOL_PARAM_STR("content",
            "Text content (newlines "
            "create paragraphs)")
            TOOL_SCHEMA_END_REQUIRED("\"document_id\",\"content\""),
        tool_feishu_doc_write_execute);

    REGISTER_TOOL(
        "feishu_doc_read",
        "Read plain-text content of a Feishu document.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("document_id", "Feishu document ID")
            TOOL_SCHEMA_END_REQUIRED("\"document_id\""),
        tool_feishu_doc_read_execute);

    REGISTER_TOOL("feishu_doc_list",
        "List recent documents in a Feishu folder.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("folder_token", "Folder token (optional)")
                TOOL_SCHEMA_END(),
        tool_feishu_doc_list_execute);

    /* Feishu group chat tools */
    REGISTER_TOOL(
        "feishu_chat_members",
        "List members of a Feishu group chat with name and open_id.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("chat_id", "Feishu group chat ID (oc_xxx)")
                TOOL_SCHEMA_END_REQUIRED("\"chat_id\""),
        tool_feishu_chat_members_execute);

    REGISTER_TOOL_CONFIRM(
        "feishu_send_mention",
        "Send @mention message in a Feishu group chat. "
        "Call ONCE per request. Use chat_id from current session.",
        TOOL_SCHEMA_BEGIN() TOOL_PARAM_STR("chat_id", "Feishu group chat ID "
                                                      "(oc_xxx)") "," TOOL_PARAM_STR("open_id",
            "Target user's open_id (ou_xxx)") "," TOOL_PARAM_STR("name",
            "Display name "
            "for the "
            "@mention") "," TOOL_PARAM_STR("text",
            "Message text after the @mention")
            TOOL_SCHEMA_END_REQUIRED("\"chat_id\",\"open_id\",\"text\""),
        tool_feishu_send_mention_execute);

    /* Music search */
    REGISTER_TOOL("music_search",
        "Search songs by keyword. Returns name, artist, URL. "
        "Does NOT play — call music_play with URL after.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("keyword", "Song name or artist to search")
            TOOL_SCHEMA_END_REQUIRED("\"keyword\""),
        tool_music_search_execute);

    /* Music playback tools (URL mode) */
    REGISTER_TOOL("music_play",
        "Play music from URL or local file. Stops current track first.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("url", "Music URL or local file path") "," TOOL_PARAM_BOOL("autostart", "Start immediately (default true)") "," TOOL_PARAM_NUM("start_position_ms", "Seek position before start (default 0)") "," TOOL_PARAM_STR(
                "options", "Media format options for raw files, e.g. "
                           "format=s16le:sample_rate=16000:ch_layout="
                           "mono") "," TOOL_PARAM_STR("format",
                "PCM sample "
                "format: s16le, "
                "s32le, f32le, "
                "etc. (default "
                "s16le)") "," TOOL_PARAM_NUM("sample_rate",
                "Sample rate in Hz, e.g. 16000, 44100, 48000") "," TOOL_PARAM_NUM("channels",
                "Number of audio channels, 1=mono, 2=stereo")
                TOOL_SCHEMA_END_REQUIRED("\"url\""),
        tool_music_play_execute);

    REGISTER_TOOL_NO_PARAMS("music_pause",
        "Pause the currently playing music track.",
        tool_music_pause_execute);

    REGISTER_TOOL_NO_PARAMS(
        "music_resume", "Resume a paused music track, or start a prepared track.",
        tool_music_resume_execute);

    REGISTER_TOOL_NO_PARAMS("music_stop",
        "Stop music playback and release all resources.",
        tool_music_stop_execute);

    REGISTER_TOOL(
        "music_seek", "Seek to a specific position in the current music track.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_NUM("position_ms", "Target position in milliseconds")
                TOOL_SCHEMA_END_REQUIRED("\"position_ms\""),
        tool_music_seek_execute);

    REGISTER_TOOL("music_set_volume",
        "Set the music stream volume. Range 0 (mute) to 100 (max).",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_NUM("volume", "Volume level 0-100")
                TOOL_SCHEMA_END_REQUIRED("\"volume\""),
        tool_music_set_volume_execute);

    REGISTER_TOOL_NO_PARAMS("music_status",
        "Get current music playback status including state, "
        "position, duration, volume, and track URL.",
        tool_music_status_execute);

    /* QuickApp launch tool */
    REGISTER_TOOL("launch_quickapp",
        "Launch a QuickApp by package name.",
        TOOL_SCHEMA_BEGIN()
            TOOL_PARAM_STR("package_name", "The QuickApp package name to launch")
                TOOL_SCHEMA_END_REQUIRED("\"package_name\""),
        tool_launch_quickapp_execute);

    /* QuickApp exit tool */
    REGISTER_TOOL_NO_PARAMS("exit_quickapp",
        "Exit current QuickApp, return to home.",
        tool_exit_quickapp_execute);

    build_tools_json_locked();
    syslog(LOG_INFO, "[%s] Tool registry initialized\n", TAG);
    return OK;
}

char* tool_registry_get_tools_json(void)
{
    char* copy;

    pthread_mutex_lock(&s_tools_mtx);
    build_tools_json_locked();
    copy = s_tools_json ? strdup(s_tools_json) : NULL;
    pthread_mutex_unlock(&s_tools_mtx);
    return copy;
}

void tool_registry_invalidate(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    s_tools_dirty = true;
    pthread_mutex_unlock(&s_tools_mtx);
}

void tool_registry_cleanup(void)
{
    pthread_mutex_lock(&s_tools_mtx);
    free(s_tools_json);
    s_tools_json = NULL;
    s_tool_count = 0;
    pthread_mutex_unlock(&s_tools_mtx);
}

bool tool_registry_requires_confirmation(const char *name)
{
    if (!name)
        return false;

    pthread_mutex_lock(&s_tools_mtx);
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            bool required = s_tools[i].voice_confirmation_required;
            pthread_mutex_unlock(&s_tools_mtx);
            return required;
        }
    }
    pthread_mutex_unlock(&s_tools_mtx);

    /* Tools supplied by MCP or dynamic providers do not yet carry trusted
     * local risk metadata.  Voice must fail safe and ask for confirmation. */
    return true;
}

int tool_registry_execute(const char *name, const char *input_json,
    char *output, size_t output_size)
{
    /* Security guard check before any tool execution */
    size_t input_len = input_json ? strlen(input_json) : 0;
    tool_guard_result_t guard = tool_guard_check(name, input_json, input_len);

    if (guard != GUARD_ALLOW) {
        const char *reason = "unknown";
        switch (guard) {
        case GUARD_DENY_DISABLED:
            reason = "tool disabled by config";
            break;
        case GUARD_DENY_RATE_LIMIT:
            reason = "rate limit exceeded";
            break;
        case GUARD_DENY_INPUT_SIZE:
            reason = "input too large";
            break;
        case GUARD_DENY_INPUT_INVALID:
            reason = "invalid input";
            break;
        default:
            break;
        }
        syslog(LOG_WARNING, "[%s] Tool '%s' blocked: %s\n",
               TAG, name ? name : "(null)", reason);
        snprintf(output, output_size,
                 "Error: tool '%s' blocked — %s",
                 name ? name : "(null)", reason);
        return ERROR;
    }

    /* Try builtin tools first */
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            syslog(LOG_INFO, "[%s] Executing tool: %s\n", TAG, name);
            int ret = s_tools[i].execute(input_json, output, output_size);
            if (ret == OK) {
                tool_guard_record_call(name);
            }
            return ret;
        }
    }

    /* Try MCP bridge */
#ifdef CONFIG_AI_AGENT_MCP
    if (mcp_bridge_execute(name, input_json, output, output_size) == OK) {
        syslog(LOG_INFO, "[%s] Executed MCP tool: %s\n", TAG, name);
        tool_guard_record_call(name);
        return OK;
    }
#endif

    /* Try registered providers */
    for (int p = 0; p < s_provider_count; p++) {
        if (!s_providers[p].execute) {
            continue;
        }
        int ret = s_providers[p].execute(name, input_json, output, output_size);
        if (ret == OK) {
            syslog(LOG_INFO, "[%s] Executed %s tool: %s\n",
                   TAG, s_providers[p].name, name);
            tool_guard_record_call(name);
            return OK;
        }
    }

    syslog(LOG_WARNING, "[%s] Unknown tool: %s\n", TAG, name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ERROR;
}
