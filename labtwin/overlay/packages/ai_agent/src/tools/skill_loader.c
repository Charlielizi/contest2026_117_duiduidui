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

#include "tools/skill_loader.h"
#include "tools/tool_registry.h"
#include "agent_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read " AGENT_MEMORY_DIR "/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for AI Agent.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `" AGENT_SKILLS_DIR "<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"" AGENT_SKILLS_DIR "translate.md\" content=\"# Translate\\n\\nTranslate text between languages.\\n\\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

#define BUILTIN_SYSTEM_HEALTH \
    "# System Health Check\n\n" \
    "Check AI Agent system status and summarize key info.\n\n" \
    "## When to use\n" \
    "When user asks about system status, health check, or running state.\n\n" \
    "## How to use\n" \
    "1. get_current_time to get current time\n" \
    "2. list_dir to list /data/agent/ files\n" \
    "3. read_file /data/agent/config/config.json to check config\n" \
    "4. cron_list to check scheduled tasks\n" \
    "5. Summarize: time, file count, config status, cron jobs\n"

#define BUILTIN_REMINDER \
    "# Reminder\n\n" \
    "Set timed reminders that auto-notify the user.\n\n" \
    "## When to use\n" \
    "When user says remind me, set alarm, notify me later.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current epoch\n" \
    "2. Parse user request into schedule_type and timing\n" \
    "3. Set channel/chat_id matching the message source (feishu/system)\n" \
    "4. cron_add to create the job\n" \
    "5. Confirm with trigger time\n"

#define BUILTIN_NOTE_TAKER \
    "# Note Taker\n\n" \
    "Quick notes saved to daily diary files.\n\n" \
    "## When to use\n" \
    "ONLY when user explicitly asks to take a note, save a memo, or record something.\n" \
    "Do NOT auto-save notes for other tasks (weather, search, etc.).\n\n" \
    "## How to use\n" \
    "1. get_current_time for today's date\n" \
    "2. Path: " AGENT_DATA_DIR "/memory/daily/YYYY-MM-DD.md\n" \
    "3. read_file to check if today's diary exists\n" \
    "4. If exists: edit_file to append. If not: write_file to create\n" \
    "5. Format: - [HH:MM] content\n"

#define BUILTIN_TRANSLATE \
    "# Translate\n\n" \
    "Translate text between languages.\n\n" \
    "## When to use\n" \
    "When user asks to translate text.\n\n" \
    "## How to use\n" \
    "1. Identify source and target languages\n" \
    "2. Translate using language knowledge\n" \
    "3. For specialized terms, use web_search to verify\n" \
    "4. Provide translation with key term notes if needed\n"

#define BUILTIN_NEWS_DIGEST \
    "# News Digest\n\n" \
    "Search and compile news summaries based on user interests.\n\n" \
    "## When to use\n" \
    "When user asks about recent news, headlines, or latest updates on a topic.\n\n" \
    "## How to use\n" \
    "1. get_current_time for current date\n" \
    "2. Determine search keywords from user request or MEMORY.md interests\n" \
    "3. news_search for relevant news (top_headlines=true for headlines)\n" \
    "4. web_search to supplement if needed\n" \
    "5. Compile 3-5 items: title, source, one-line summary\n"

#define BUILTIN_FEISHU_TEST \
    "# Feishu Integration Test\n\n" \
    "Test Feishu Bot capabilities end-to-end.\n\n" \
    "## When to use\n" \
    "When user says test feishu, feishu test, or verify feishu connection.\n\n" \
    "## How to use\n" \
    "Run these tests in sequence, report each result:\n" \
    "1. get_current_time - verify time\n" \
    "2. get_weather location=Beijing - verify weather\n" \
    "3. write_file + read_file a test file - verify file I/O\n" \
    "4. read_file " AGENT_DATA_DIR "/memory/MEMORY.md - verify memory\n" \
    "5. cron_list - verify cron\n" \
    "6. Summarize all results with pass/fail status\n"

#define BUILTIN_TASK_MANAGER \
    "# Task Manager\n\n" \
    "Manage a TODO list with add, complete, and view.\n\n" \
    "## When to use\n" \
    "When user says add task, TODO, done with X, what's pending.\n\n" \
    "## How to use\n" \
    "Task file: " AGENT_DATA_DIR "/TASKS.md\n" \
    "- View: read_file the task file\n" \
    "- Add: get_current_time, then edit_file/write_file to append: - [ ] [YYYY-MM-DD] desc\n" \
    "- Complete: edit_file to change - [ ] to - [x]\n"

#define BUILTIN_LABTWIN_V1 \
    "# LabTwin Voice v1\n\n" \
    "Operate the local laboratory runtime from one spoken command.\n\n" \
    "## Rules\n" \
    "- Use only experiment_create/get/list/transition, timer_start/cancel, " \
    "lab_log_add, and sensor_snapshot for laboratory facts.\n" \
    "- One utterance may request at most one mutating tool call.\n" \
    "- Never invent an experiment ID, step, duration, observation, or unit.\n" \
    "- If an ID, duration, or target is missing or ambiguous, ask a question " \
    "and do not call a mutating tool.\n" \
    "- Voice mutating calls are proposals. Say that confirmation is required; " \
    "never say the change was applied until the tool returns OK after confirmation.\n" \
    "- For lab_log_add preserve the verbatim transcript in text. Structured " \
    "facts use {step_index, facts:[{key,value,unit?}]}; omit uncertain facts.\n"

#define BUILTIN_LABTWIN \
    "# LabTwin Voice v2\n\n" \
    "Use the local laboratory runtime for experiment state and device facts; " \
    "answer ordinary knowledge questions normally. Keep spoken replies concise.\n\n" \
    "## Tools\n" \
    "- Read-only: experiment_get, experiment_list, sensor_snapshot, " \
    "environment_event_list. These do not need confirmation.\n" \
    "- Mutating: experiment_create, experiment_transition, timer_start, " \
    "timer_cancel, lab_log_add, environment_event_ack. Voice calls to these " \
    "are proposals and require on-device confirmation; non-voice calls execute " \
    "directly after the matching tool returns OK.\n\n" \
    "## Resolve the target\n" \
    "- Never invent an existing experiment ID, timer ID, step, duration, " \
    "observation, event ID, or unit.\n" \
    "- If the user refers to the current experiment without an ID, call " \
    "experiment_list. If exactly one active experiment matches, use its ID; " \
    "otherwise ask a short clarification question.\n" \
    "- experiment_create may omit experiment_id so the device generates one. " \
    "A name and at least one explicit step are required; ask for missing steps. " \
    "Preserve a requested description in description. For an explicit schedule, " \
    "call get_current_time when needed to resolve relative dates, then pass " \
    "planned_start_epoch and planned_end_epoch as Unix seconds. Never put " \
    "description or schedule in an observation instead.\n\n" \
    "## Execute safely\n" \
    "- One utterance may contain read calls followed by at most one mutating " \
    "tool call. Never bundle create-and-start or multiple writes.\n" \
    "- Before a mutating call, preserve the user's requested values exactly. " \
    "For voice, make a proposal and ask for on-device confirmation. For web " \
    "and other non-voice sessions, emit the matching tool call before any " \
    "success statement; a natural-language plan is never an executed action.\n" \
    "- Never claim a change was applied while the tool result says " \
    "PENDING_CONFIRMATION. Report success only after an OK result.\n" \
    "- For lab_log_add preserve the verbatim transcript in text. Structured " \
    "facts use {step_index, facts:[{key,value,unit?}]}; omit uncertain facts.\n" \
    "- environment_event_ack only records acknowledgement; it never claims " \
    "that the physical condition is safe or cleared.\n" \
    "- Do not read raw JSON aloud. Summarize the result in natural Chinese " \
    "and mention IDs only when needed for the next action.\n"

/* Keep in sync with agent_skills/chemistry-experiment-assistant.md. */
#define BUILTIN_CHEMISTRY_EXPERIMENT_ASSISTANT \
    "# Chemistry Experiment Assistant\n\n" \
    "Answer chemistry questions and plan laboratory work. Keep planned, " \
    "confirmed, and measured facts separate. This skill does not replace " \
    "local SOPs, EHS review, or trained operators.\n\n" \
    "## When to use\n" \
    "Use for chemistry concepts, reaction mechanisms, stoichiometry, dilution, " \
    "pH or buffer calculations, experiment task planning, reagent and equipment " \
    "scheduling, sample tracking, or laboratory records. Reply in the user's language.\n\n" \
    "## Safety gate\n" \
    "1. Before a concrete experiment plan, identify substances, scale, SDS and " \
    "local SOP status, hazards, incompatible materials, equipment limits, waste, " \
    "PPE, ventilation, approvals, and operator qualification.\n" \
    "2. For unknown materials, explosive or pressure risks, strong exotherms, " \
    "highly toxic or corrosive materials, or any request to bypass safeguards, " \
    "do not give an executable procedure. State the risk and require EHS or " \
    "qualified-supervisor review.\n" \
    "3. Never claim a safety check, experiment action, instrument reading, or " \
    "measurement occurred without user-provided evidence or a successful tool result.\n\n" \
    "## Knowledge questions\n" \
    "1. Lead with the answer, then explain the relevant definition, balanced " \
    "equation, mechanism, or calculation.\n" \
    "2. Show units, assumptions, significant figures, and limits of any formula. " \
    "For stoichiometry, identify the limiting reagent and distinguish theoretical " \
    "from actual yield.\n" \
    "3. For current regulations, SDS details, or requested citations, use reliable " \
    "sources when available; do not invent data or references.\n\n" \
    "## Plan and schedule\n" \
    "1. Confirm the objective, success criterion, sample and reagent identity, " \
    "scale, repetitions or controls, deadline, available personnel, equipment, " \
    "and time windows. List missing information as assumptions.\n" \
    "2. Break the work into tasks with an ID, active and waiting time, prerequisites, " \
    "resource owner, completion criterion, stop condition, and fallback. Include " \
    "preflight, calibration, controls, cleanup, waste transfer, and data backup.\n" \
    "3. Parallelize only when resources and chemical hazards are compatible and " \
    "the laboratory can still respond to an incident. Mark the critical path and buffer.\n" \
    "4. Present a schedule with goals, assumptions, safety gates, task table, " \
    "critical path, records, and open questions. Use relative time if no dated " \
    "start time or timezone is supplied.\n\n" \
    "## LabTwin integration\n" \
    "- Ordinary questions and draft plans need no tool call.\n" \
    "- For experiment facts, use the LabTwin read tools rather than guessing.\n" \
    "- For a real schedule, status change, timer, observation, or equipment action, " \
    "follow the lab-twin skill: resolve the target, preserve user values, require " \
    "the applicable confirmation, and progress only through the validated protocol.\n" \
    "- Do not turn a draft chemistry plan into an executed experiment until its SOP, " \
    "parameters, safety checks, and operator confirmation are complete.\n\n" \
    "## Example\n" \
    "User: Plan an acid-base titration for next week.\n" \
    "First collect titrant concentration, sample count, instrument availability, " \
    "and safety details; then return a proposed calibration, blank, titration, " \
    "cleanup, and data-review schedule.\n"

#define BUILTIN_LAB_MEMORY_REPORTS \
    "# Laboratory Memory and Reports\n\n" \
    "Use confirmed structured memory for reusable laboratory context and " \
    "report cards for historical performance.\n\n" \
    "## Memory\n" \
    "- Only propose stable, reusable information: research direction, method, " \
    "instrument convention, terminology, recording/safety boundary, or the " \
    "current user's language, units, or response style.\n" \
    "- Never store raw measurements, one-off observations, SOP parameters, " \
    "credentials, personal sensitive data, or unsafe operational details.\n" \
    "- Call memory_propose once, summarize the exact proposed item, and ask for " \
    "确认保存 or /memory confirm. A proposal is not saved memory.\n\n" \
    "## Reports\n" \
    "- report_create only records user-provided summary and numeric metrics. " \
    "Use experiment_id only after resolving it. Corrections create a new card " \
    "with supersedes_report_id; never overwrite a card.\n" \
    "- For earlier experimental performance, highest/lowest metrics, or historical " \
    "conclusions, call report_search before answering. Cite report_id, metric, " \
    "unit, and condition. Never call a planned or inferred value a measurement.\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
    { "system-health",  BUILTIN_SYSTEM_HEALTH  },
    { "reminder",       BUILTIN_REMINDER       },
    { "note-taker",     BUILTIN_NOTE_TAKER     },
    { "translate",      BUILTIN_TRANSLATE      },
    { "news-digest",    BUILTIN_NEWS_DIGEST    },
    { "feishu-test",    BUILTIN_FEISHU_TEST    },
    { "task-manager",   BUILTIN_TASK_MANAGER   },
    { "lab-twin",       BUILTIN_LABTWIN        },
    /* NuttX NAME_MAX is 32, including the .md suffix added at install. */
    { "chemistry-lab",
      BUILTIN_CHEMISTRY_EXPERIMENT_ASSISTANT },
    { "laboratory-memory-reports", BUILTIN_LAB_MEMORY_REPORTS },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static int file_matches(FILE *f, const char *expected)
{
    char buffer[128];
    size_t offset = 0;
    size_t expected_size = strlen(expected);
    size_t count;

    rewind(f);
    while ((count = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (count > expected_size - offset ||
            memcmp(buffer, expected + offset, count) != 0) return 0;
        offset += count;
    }
    return !ferror(f) && offset == expected_size;
}

static void install_builtin(const builtin_skill_t *skill)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, skill->filename);

    /* Check if already exists */
    FILE *f = fopen(path, "r");
    if (f) {
        int upgrade = strcmp(skill->filename, "lab-twin") == 0 &&
                      file_matches(f, BUILTIN_LABTWIN_V1);
        fclose(f);
        if (!upgrade) {
            syslog(LOG_DEBUG, "[%s] Skill exists: %s\n", TAG, path);
            return;
        }
        syslog(LOG_INFO, "[%s] Upgrading bundled skill: %s\n", TAG, path);
    }

    /* Write built-in skill */
    f = fopen(path, "w");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot write skill: %s\n", TAG, path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    syslog(LOG_INFO, "[%s] Installed built-in skill: %s\n", TAG, path);
}

int skill_loader_init(void)
{
    syslog(LOG_INFO, "[%s] Initializing skills system\n", TAG);

    /* Ensure skills directory exists */
    mkdir(AGENT_SKILLS_DIR, 0755);

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    syslog(LOG_INFO, "[%s] Skills system ready (%d built-in)\n", TAG, (int)NUM_BUILTINS);
    return OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return 0;
    }

    buf[0] = '\0';

    /*
     * On Vela/NuttX we have real directories, so we can simply opendir
     * on the skills directory and iterate over .md files.
     * (Original version used flat namespace readdir from mount root.)
     */
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        syslog(LOG_WARNING, "[%s] Cannot open skills directory for enumeration: %s\n", TAG, AGENT_SKILLS_DIR);
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        /* Match .md files only */
        if (name_len < 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Skip hidden files */
        if (name[0] == '.') continue;

        /* Build full path */
        char full_path[256];
        snprintf(full_path, sizeof(full_path), "%s%s", AGENT_SKILLS_DIR, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary without advancing past the output buffer. */
        size_t remaining = size - off;
        int written = snprintf(buf + off, remaining,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= remaining) {
            off = size - 1;
            break;
        }
        off += (size_t)written;
    }

    closedir(dir);

    buf[off] = '\0';
    syslog(LOG_INFO, "[%s] Skills summary: %d bytes\n", TAG, (int)off);
    return off;
}

/* ── Hot-reload support ──────────────────────────────────────── */

static uint32_t s_last_skill_hash;

/* Simple hash of directory listing: file count + total size */
static uint32_t compute_skills_hash(void)
{
    DIR *dir = opendir(AGENT_SKILLS_DIR);
    if (!dir) {
        return 0;
    }

    uint32_t hash = 5381;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (name_len < 4 || strcmp(name + name_len - 3, ".md") != 0) {
            continue;
        }
        if (name[0] == '.') {
            continue;
        }

        /* Hash filename */
        for (size_t i = 0; i < name_len; i++) {
            hash = ((hash << 5) + hash) + (unsigned char)name[i];
        }

        /* Hash file size + mtime (detects content changes) */
        char path[256];
        snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR, name);
        struct stat st;
        if (stat(path, &st) == 0) {
            hash = ((hash << 5) + hash) + (uint32_t)st.st_size;
            hash = ((hash << 5) + hash) + (uint32_t)st.st_mtime;
        }
    }

    closedir(dir);
    return hash;
}

bool skill_loader_check_changed(void)
{
    uint32_t current = compute_skills_hash();
    if (s_last_skill_hash == 0) {
        s_last_skill_hash = current;
        return false;
    }
    if (current != s_last_skill_hash) {
        s_last_skill_hash = current;
        return true;
    }
    return false;
}

void skill_loader_refresh(void)
{
    s_last_skill_hash = compute_skills_hash();
    /* Invalidate tool registry so next get_tools_json rebuilds */
    tool_registry_invalidate();
    syslog(LOG_INFO, "[%s] Skills refreshed (hash=%08x)\n",
        TAG, s_last_skill_hash);
}
