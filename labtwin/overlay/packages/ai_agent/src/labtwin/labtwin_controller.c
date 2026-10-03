#include "labtwin/labtwin_controller.h"
#include "labtwin/labtwin.h"
#include "labtwin/labtwin_protocol.h"

#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LABTWIN_ROOT
#if defined(LABTWIN_CONTROLLER_HOST_TEST) || defined(LABTWIN_HOST_TEST)
#define LABTWIN_ROOT "/tmp/labtwin-controller-v1-test"
#else
#define LABTWIN_ROOT "/data/labtwin"
#endif
#endif

#define CONTROLLER_DIR LABTWIN_ROOT "/controllers"
#define RUN_ID_MAX 32
#define PROTOCOL_ID_MAX 32
#define EXPERIMENT_ID_MAX 32
#define PHASE_MAX 20
#define REASON_MAX 128
#define PATH_MAX_LOCAL 192
#define PROTOCOL_RESPONSE_SIZE (LABTWIN_PROTOCOL_MAX_JSON_SIZE + 1024)

typedef enum {
    PHASE_PLAN = 0,
    PHASE_EXECUTE,
    PHASE_OBSERVE,
    PHASE_EVALUATE,
    PHASE_PAUSED,
    PHASE_NEEDS_HELP,
    PHASE_COMPLETED,
    PHASE_ABORTED,
    PHASE_INVALID
} controller_phase_t;

static pthread_mutex_t g_controller_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_controller_initialized;

static bool valid_evidence(const cJSON *evidence);

static const char *phase_name(controller_phase_t phase)
{
    static const char *const names[] = {
        "PLAN", "EXECUTE", "OBSERVE", "EVALUATE", "PAUSED",
        "NEEDS_HELP", "COMPLETED", "ABORTED"
    };
    return phase >= PHASE_PLAN && phase <= PHASE_ABORTED ? names[phase] : "INVALID";
}

static controller_phase_t parse_phase(const cJSON *item)
{
    if (!cJSON_IsString(item)) {
        return PHASE_INVALID;
    }
    for (int phase = PHASE_PLAN; phase <= PHASE_ABORTED; phase++) {
        if (strcmp(item->valuestring, phase_name((controller_phase_t)phase)) == 0) {
            return (controller_phase_t)phase;
        }
    }
    return PHASE_INVALID;
}

static bool active_phase(controller_phase_t phase)
{
    return phase >= PHASE_PLAN && phase <= PHASE_EVALUATE;
}

static int mkdir_checked(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST) {
        return -errno;
    }
    chmod(path, 0700);
    return 0;
}

static bool valid_identifier(const char *value, size_t max_len)
{
    size_t length;

    if (!value || (length = strlen(value)) == 0 || length >= max_len) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)value[i];
        if (!isalnum(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

static bool valid_integer(const cJSON *item, int minimum, int maximum)
{
    double value;

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    value = item->valuedouble;
    return value >= minimum && value <= maximum && value == (int)value;
}

static bool fields_allowed(const cJSON *object,
                           const char *const *allowed, size_t count)
{
    const cJSON *item;

    if (!cJSON_IsObject(object)) {
        return false;
    }
    cJSON_ArrayForEach(item, object) {
        bool found = false;
        for (size_t i = 0; i < count; i++) {
            if (item->string && strcmp(item->string, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static int emit_response(char *output, size_t output_size, bool ok,
                         const char *code, const char *message, cJSON *data)
{
    cJSON *root;
    char *serialized;
    size_t length;
    int result = 0;

    if (!output || output_size == 0) {
        cJSON_Delete(data);
        return -EINVAL;
    }
    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(data);
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"NO_MEMORY\"}");
        return -ENOMEM;
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message);
    if (data) {
        cJSON_AddItemToObject(root, "data", data);
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) {
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"NO_MEMORY\"}");
        return -ENOMEM;
    }
    length = strlen(serialized);
    if (length >= output_size) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"code\":\"OUTPUT_TOO_SMALL\"}");
        result = -ENOSPC;
    } else {
        memcpy(output, serialized, length + 1);
    }
    free(serialized);
    return result;
}

static int fail_response(char *output, size_t output_size, int error,
                         const char *code, const char *message)
{
    int emit_result = emit_response(output, output_size, false, code, message, NULL);
    return emit_result != 0 ? emit_result : error;
}

static void run_paths(const char *run_id, char *snapshot, size_t snapshot_size,
                      char *events, size_t events_size)
{
    snprintf(snapshot, snapshot_size, "%s/%s.json", CONTROLLER_DIR, run_id);
    snprintf(events, events_size, "%s/%s.events.jsonl", CONTROLLER_DIR, run_id);
}

static cJSON *read_json_file(const char *path)
{
    FILE *file = fopen(path, "r");
    char *buffer;
    long length;
    cJSON *document;

    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 ||
        length > LABTWIN_CONTROLLER_MAX_JSON_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[length] = 0;
    fclose(file);
    document = cJSON_Parse(buffer);
    free(buffer);
    return document;
}

static int snapshot_write(const char *path, const cJSON *run)
{
    char temporary[PATH_MAX_LOCAL + 8];
    char *serialized = cJSON_PrintUnformatted(run);
    FILE *file;
    int failed = 0;

    if (!serialized) {
        return -ENOMEM;
    }
    if (strlen(serialized) >= LABTWIN_CONTROLLER_MAX_JSON_SIZE) {
        free(serialized);
        return -E2BIG;
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "w");
    if (!file) {
        free(serialized);
        return -errno;
    }
    if (fprintf(file, "%s\n", serialized) < 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    free(serialized);
    if (failed) {
        remove(temporary);
        return -EIO;
    }
    if (rename(temporary, path) != 0) {
        remove(temporary);
        return -errno;
    }
    return 0;
}

static int event_append(const char *path, const cJSON *run,
                        const char *event_type, const char *source)
{
    cJSON *event = cJSON_CreateObject();
    char *serialized;
    FILE *file;
    int failed = 0;

    if (!event) {
        return -ENOMEM;
    }
    cJSON_AddStringToObject(event, "event_type", event_type);
    cJSON_AddStringToObject(event, "source", source ? source : "unknown");
    cJSON_AddItemToObject(event, "state", cJSON_Duplicate(run, true));
    serialized = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!serialized) {
        return -ENOMEM;
    }
    if (strlen(serialized) >= LABTWIN_CONTROLLER_MAX_JSON_SIZE) {
        free(serialized);
        return -E2BIG;
    }
    file = fopen(path, "a");
    if (!file) {
        free(serialized);
        return -errno;
    }
    if (fprintf(file, "%s\n", serialized) < 0) {
        failed = 1;
    }
    if (fclose(file) != 0) {
        failed = 1;
    }
    free(serialized);
    return failed ? -EIO : 0;
}

static unsigned int run_sequence(const cJSON *run)
{
    const cJSON *item = cJSON_GetObjectItem(run, "last_seq");
    return valid_integer(item, 0, INT32_MAX) ? (unsigned int)item->valueint : 0;
}

static bool valid_run_state(const cJSON *run)
{
    static const char *const allowed[] = {
        "schema_version", "run_id", "experiment_id", "protocol_id",
        "protocol_version", "phase", "resume_phase", "scope",
        "current_step", "step_count", "attempt", "last_seq", "last_reason",
        "parameters", "last_evidence"
    };
    const cJSON *experiment_id;
    const cJSON *scope;
    const cJSON *current_step;
    const cJSON *step_count;
    const cJSON *reason;
    controller_phase_t phase;
    controller_phase_t resume_phase;

    if (!fields_allowed(run, allowed, sizeof(allowed) / sizeof(allowed[0])) ||
        !valid_integer(cJSON_GetObjectItem(run, "schema_version"),
                       LABTWIN_CONTROLLER_SCHEMA_VERSION,
                       LABTWIN_CONTROLLER_SCHEMA_VERSION) ||
        !cJSON_IsString(cJSON_GetObjectItem(run, "run_id")) ||
        !valid_identifier(cJSON_GetObjectItem(run, "run_id")->valuestring,
                          RUN_ID_MAX) ||
        !cJSON_IsString(experiment_id = cJSON_GetObjectItem(run, "experiment_id")) ||
        (experiment_id->valuestring[0] != 0 &&
         !valid_identifier(experiment_id->valuestring, EXPERIMENT_ID_MAX)) ||
        !cJSON_IsString(cJSON_GetObjectItem(run, "protocol_id")) ||
        !valid_identifier(cJSON_GetObjectItem(run, "protocol_id")->valuestring,
                          PROTOCOL_ID_MAX) ||
        !valid_integer(cJSON_GetObjectItem(run, "protocol_version"), 1, UINT16_MAX) ||
        (phase = parse_phase(cJSON_GetObjectItem(run, "phase"))) == PHASE_INVALID ||
        (resume_phase = parse_phase(cJSON_GetObjectItem(run, "resume_phase"))) == PHASE_INVALID ||
        !active_phase(resume_phase) ||
        !cJSON_IsString(scope = cJSON_GetObjectItem(run, "scope")) ||
        (strcmp(scope->valuestring, "prerequisites") != 0 &&
         strcmp(scope->valuestring, "step") != 0 &&
         strcmp(scope->valuestring, "acceptance") != 0) ||
        !valid_integer(current_step = cJSON_GetObjectItem(run, "current_step"),
                       0, 16) ||
        !valid_integer(step_count = cJSON_GetObjectItem(run, "step_count"), 1, 16) ||
        current_step->valueint > step_count->valueint ||
        !valid_integer(cJSON_GetObjectItem(run, "attempt"), 0, 3) ||
        !valid_integer(cJSON_GetObjectItem(run, "last_seq"), 1, INT32_MAX) ||
        !cJSON_IsString(reason = cJSON_GetObjectItem(run, "last_reason")) ||
        strlen(reason->valuestring) >= REASON_MAX ||
        !cJSON_IsObject(cJSON_GetObjectItem(run, "parameters")) ||
        !valid_evidence(cJSON_GetObjectItem(run, "last_evidence"))) {
        return false;
    }
    if (strcmp(scope->valuestring, "step") == 0 &&
        current_step->valueint >= step_count->valueint) {
        return false;
    }
    if (strcmp(scope->valuestring, "acceptance") == 0 &&
        current_step->valueint != step_count->valueint) {
        return false;
    }
    if (phase == PHASE_PLAN && strcmp(scope->valuestring, "prerequisites") != 0) {
        return false;
    }
    return true;
}

static cJSON *recover_latest_event(const char *path, unsigned int after_seq)
{
    FILE *file = fopen(path, "r");
    char *line;
    cJSON *latest = NULL;
    unsigned int latest_seq = after_seq;

    if (!file) {
        return NULL;
    }
    line = malloc(LABTWIN_CONTROLLER_MAX_JSON_SIZE + 2);
    if (!line) {
        fclose(file);
        return NULL;
    }
    while (fgets(line, LABTWIN_CONTROLLER_MAX_JSON_SIZE + 2, file)) {
        cJSON *event;
        const cJSON *state;
        unsigned int sequence;

        if (!strchr(line, '\n')) {
            continue;
        }
        event = cJSON_Parse(line);
        state = event ? cJSON_GetObjectItem(event, "state") : NULL;
        sequence = valid_run_state(state) ? run_sequence(state) : 0;
        if (sequence > latest_seq) {
            cJSON_Delete(latest);
            latest = cJSON_Duplicate(state, true);
            latest_seq = sequence;
        }
        cJSON_Delete(event);
    }
    free(line);
    fclose(file);
    return latest;
}

static cJSON *load_run_locked(const char *run_id)
{
    char snapshot_path[PATH_MAX_LOCAL];
    char event_path[PATH_MAX_LOCAL];
    cJSON *run;
    cJSON *recovered;

    run_paths(run_id, snapshot_path, sizeof(snapshot_path),
              event_path, sizeof(event_path));
    run = read_json_file(snapshot_path);
    if (run && !valid_run_state(run)) {
        cJSON_Delete(run);
        run = NULL;
    }
    recovered = recover_latest_event(event_path, run ? run_sequence(run) : 0);
    if (recovered) {
        cJSON_Delete(run);
        run = recovered;
        snapshot_write(snapshot_path, run);
    }
    return run;
}


/* ── Experiment step synchronization ───────────────────────────────── */

/*
 * If the controller run is linked to an experiment_id, sync the current
 * state to the legacy experiment_steps model.  This keeps dashboards,
 * voice commands and the experiment API consistent with the controller.
 *
 * The controller remains the source of truth; the experiment copy is a
 * read-friendly projection.  We never read from the experiment back into
 * the controller.
 *
 * Synchronization is best-effort: failures here never fail the controller
 * operation itself.
 */

static int sync_create_experiment(const cJSON *run, const cJSON *protocol,
                                  const char *source)
{
    const cJSON *experiment_id = cJSON_GetObjectItem(run, "experiment_id");
    const cJSON *steps;
    cJSON *payload;
    cJSON *existing = NULL;
    cJSON *data;
    cJSON *existing_steps;
    char *payload_str;
    char result[4096];
    int ret;

    if (!experiment_id || !cJSON_IsString(experiment_id) ||
        experiment_id->valuestring[0] == (char)0 || !protocol) {
        return 0;
    }

    /* Existing experiments are eligible only when they remain an exact,
     * unstarted projection of the immutable protocol. */
    char get_payload[128];
    char get_result[4096];
    snprintf(get_payload, sizeof(get_payload),
             "{\"experiment_id\":\"%s\"}", experiment_id->valuestring);
    ret = labtwin_experiment_get_json(get_payload, get_result,
                                      sizeof(get_result));
    if (ret == 0) {
        existing = cJSON_Parse(get_result);
        data = existing ? cJSON_GetObjectItem(existing, "data") : NULL;
        existing_steps = data ? cJSON_GetObjectItem(data, "steps") : NULL;
        steps = cJSON_GetObjectItem(protocol, "steps");
        if (!cJSON_IsArray(existing_steps) || !cJSON_IsArray(steps) ||
            cJSON_GetArraySize(existing_steps) != cJSON_GetArraySize(steps) ||
            !cJSON_IsString(cJSON_GetObjectItem(data, "state")) ||
            strcmp(cJSON_GetObjectItem(data, "state")->valuestring,
                   "READY") != 0) {
            cJSON_Delete(existing);
            return -EINVAL;
        }
        for (int i = 0; i < cJSON_GetArraySize(steps); i++) {
            const cJSON *existing_step = cJSON_GetArrayItem(existing_steps, i);
            const cJSON *protocol_step = cJSON_GetArrayItem(steps, i);
            const char *existing_title = cJSON_GetStringValue(
                cJSON_GetObjectItem(existing_step, "title"));
            const char *protocol_title = cJSON_GetStringValue(
                cJSON_GetObjectItem(protocol_step, "title"));
            if (!existing_title || !protocol_title ||
                strcmp(existing_title, protocol_title) != 0) {
                cJSON_Delete(existing);
                return -EINVAL;
            }
        }
        cJSON_Delete(existing);
        return 0;
    }
    if (ret != -ENOENT) {
        return ret;
    }

    /* Build the legacy experiment projection from protocol step titles. */
    steps = cJSON_GetObjectItem(protocol, "steps");
    if (!steps || !cJSON_IsArray(steps)) return -EINVAL;

    payload = cJSON_CreateObject();
    if (!payload) return -ENOMEM;

    cJSON_AddStringToObject(payload, "experiment_id",
                            experiment_id->valuestring);
    cJSON_AddStringToObject(payload, "name",
                            cJSON_GetStringValue(cJSON_GetObjectItem(
                                protocol, "name")) ?
                            cJSON_GetStringValue(cJSON_GetObjectItem(
                                protocol, "name")) :
                            experiment_id->valuestring);
    {
        const char *desc = cJSON_GetStringValue(
            cJSON_GetObjectItem(protocol, "description"));
        if (desc) cJSON_AddStringToObject(payload, "description", desc);
    }
    {
        cJSON *exp_steps = cJSON_CreateArray();
        cJSON *step;
        if (!exp_steps) {
            cJSON_Delete(payload);
            return -ENOMEM;
        }
        cJSON_ArrayForEach(step, steps) {
            const char *title = cJSON_GetStringValue(
                cJSON_GetObjectItem(step, "title"));
            cJSON *projection_step = NULL;
            if (!title || !(projection_step = cJSON_CreateString(title)) ||
                !cJSON_AddItemToArray(exp_steps, projection_step)) {
                cJSON_Delete(projection_step);
                cJSON_Delete(exp_steps);
                cJSON_Delete(payload);
                return title ? -ENOMEM : -EINVAL;
            }
        }
        cJSON_AddItemToObject(payload, "steps", exp_steps);
    }

    payload_str = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!payload_str) return -ENOMEM;

    ret = labtwin_experiment_create_json(payload_str, result, sizeof(result),
                                         source);
    free(payload_str);
    return ret;
}

static int sync_experiment_step(const cJSON *run, const char *event_type,
                                const char *source)
{
    const cJSON *experiment_id = cJSON_GetObjectItem(run, "experiment_id");
    const char *action = NULL;
    controller_phase_t phase;

    if (!experiment_id || !cJSON_IsString(experiment_id) ||
        experiment_id->valuestring[0] == (char)0) {
        return 0;
    }

    /* CREATED is handled by sync_create_experiment during controller_create. */
    if (strcmp(event_type, "CREATED") == 0) return 0;

    phase = parse_phase(cJSON_GetObjectItem(run, "phase"));

    /* Map the authoritative controller event to the legacy experiment
     * projection.  The projection never supplies state back to the run. */
    if (strcmp(event_type, "PLAN_ACCEPTED") == 0) {
        action = "start";
    } else if (strcmp(event_type, "STEP_COMPLETED") == 0) {
        /* This event is emitted only after a non-final controller step has
         * advanced, so complete the legacy step unconditionally. */
        action = "complete_step";
    } else if (strcmp(event_type, "STEPS_COMPLETED") == 0) {
        /* The legacy service combines the final step with completion, so it
         * remains active until the controller acceptance criteria pass. */
    } else if (strcmp(event_type, "COMPLETED") == 0) {
        action = "complete";
    } else if (strcmp(event_type, "PAUSED") == 0) {
        action = "pause";
    } else if (strcmp(event_type, "RESUMED") == 0) {
        action = "resume";
    } else if (strcmp(event_type, "ABORTED") == 0) {
        action = "cancel";
    } else if (strcmp(event_type, "HELP_REQUESTED") == 0 ||
               strcmp(event_type, "EVALUATION_ERROR") == 0 ||
               strcmp(event_type, "TIMEOUT_HANDLED") == 0 ||
               strcmp(event_type, "FAILURE_HANDLED") == 0) {
        if (phase == PHASE_PAUSED || phase == PHASE_NEEDS_HELP) {
            action = "pause";
        } else if (phase == PHASE_ABORTED) {
            action = "cancel";
        }
    }

    if (!action) return 0;

    char payload[256];
    char result[512];
    snprintf(payload, sizeof(payload),
             "{\"experiment_id\":\"%s\",\"action\":\"%s\"}",
             experiment_id->valuestring, action);

    /* The controller state is already durable; projection errors are
     * retriable and must not roll its guarded transition back. */
    (void)labtwin_experiment_transition_json(payload, result, sizeof(result),
                                             source);
    return 0;
}
static int commit_locked(cJSON *run, const char *event_type,
                         const char *source)
{
    const cJSON *run_id = cJSON_GetObjectItem(run, "run_id");
    char snapshot_path[PATH_MAX_LOCAL];
    char event_path[PATH_MAX_LOCAL];
    unsigned int sequence = run_sequence(run) + 1;
    int result;

    cJSON_ReplaceItemInObject(run, "last_seq", cJSON_CreateNumber(sequence));
    run_paths(run_id->valuestring, snapshot_path, sizeof(snapshot_path),
              event_path, sizeof(event_path));
    result = event_append(event_path, run, event_type, source);
    if (result == 0) {
        result = snapshot_write(snapshot_path, run);
    }
    if (result == 0) {
        (void)sync_experiment_step(run, event_type, source);
    }
    return result;
}

static cJSON *load_protocol(const char *protocol_id, unsigned int version)
{
    char request[128];
    char *response = malloc(PROTOCOL_RESPONSE_SIZE);
    cJSON *envelope;
    cJSON *data;

    if (!response) {
        return NULL;
    }
    snprintf(request, sizeof(request),
             "{\"protocol_id\":\"%s\",\"version\":%u}",
             protocol_id, version);
    if (labtwin_protocol_get_json(request, response,
                                  PROTOCOL_RESPONSE_SIZE) != 0) {
        free(response);
        return NULL;
    }
    envelope = cJSON_Parse(response);
    free(response);
    data = envelope ? cJSON_DetachItemFromObject(envelope, "data") : NULL;
    cJSON_Delete(envelope);
    return data;
}

static bool primitive_equal(const cJSON *left, const cJSON *right)
{
    if (cJSON_IsNumber(left) && cJSON_IsNumber(right)) {
        return left->valuedouble == right->valuedouble;
    }
    if (cJSON_IsString(left) && cJSON_IsString(right)) {
        return strcmp(left->valuestring, right->valuestring) == 0;
    }
    if (cJSON_IsBool(left) && cJSON_IsBool(right)) {
        return cJSON_IsTrue(left) == cJSON_IsTrue(right);
    }
    return cJSON_IsNull(left) && cJSON_IsNull(right);
}

static bool parameter_type_matches(const cJSON *value, const char *type)
{
    if (strcmp(type, "string") == 0) {
        return cJSON_IsString(value);
    }
    if (strcmp(type, "integer") == 0) {
        return valid_integer(value, INT32_MIN, INT32_MAX);
    }
    if (strcmp(type, "number") == 0) {
        return cJSON_IsNumber(value);
    }
    if (strcmp(type, "boolean") == 0) {
        return cJSON_IsBool(value);
    }
    return false;
}

static cJSON *resolve_parameters(const cJSON *definitions,
                                 const cJSON *provided, char *reason,
                                 size_t reason_size)
{
    cJSON *resolved = cJSON_CreateObject();
    const cJSON *item;
    const cJSON *definition;

    if (!resolved || !cJSON_IsArray(definitions) || !cJSON_IsObject(provided)) {
        cJSON_Delete(resolved);
        snprintf(reason, reason_size, "parameters must be an object");
        return NULL;
    }
    cJSON_ArrayForEach(item, provided) {
        bool found = false;
        cJSON_ArrayForEach(definition, definitions) {
            const cJSON *name = cJSON_GetObjectItem(definition, "name");
            if (cJSON_IsString(name) && item->string &&
                strcmp(name->valuestring, item->string) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(reason, reason_size, "unknown parameter: %s",
                     item->string ? item->string : "?");
            cJSON_Delete(resolved);
            return NULL;
        }
    }
    cJSON_ArrayForEach(definition, definitions) {
        const cJSON *name = cJSON_GetObjectItem(definition, "name");
        const cJSON *type = cJSON_GetObjectItem(definition, "type");
        const cJSON *value = cJSON_GetObjectItem(provided, name->valuestring);
        const cJSON *minimum = cJSON_GetObjectItem(definition, "minimum");
        const cJSON *maximum = cJSON_GetObjectItem(definition, "maximum");
        const cJSON *values = cJSON_GetObjectItem(definition, "enum");
        const cJSON *candidate;
        bool enum_match = values == NULL;

        if (!value) {
            value = cJSON_GetObjectItem(definition, "default");
        }
        if (!value) {
            const cJSON *required = cJSON_GetObjectItem(definition, "required");
            if (cJSON_IsTrue(required)) {
                snprintf(reason, reason_size, "required parameter missing: %s",
                         name->valuestring);
                cJSON_Delete(resolved);
                return NULL;
            }
            continue;
        }
        if (!parameter_type_matches(value, type->valuestring) ||
            (minimum && value->valuedouble < minimum->valuedouble) ||
            (maximum && value->valuedouble > maximum->valuedouble)) {
            snprintf(reason, reason_size, "invalid parameter: %s", name->valuestring);
            cJSON_Delete(resolved);
            return NULL;
        }
        if (values) {
            cJSON_ArrayForEach(candidate, values) {
                if (primitive_equal(value, candidate)) {
                    enum_match = true;
                    break;
                }
            }
        }
        if (!enum_match) {
            snprintf(reason, reason_size, "parameter outside enum: %s",
                     name->valuestring);
            cJSON_Delete(resolved);
            return NULL;
        }
        cJSON_AddItemToObject(resolved, name->valuestring,
                             cJSON_Duplicate(value, true));
    }
    return resolved;
}

static bool valid_evidence(const cJSON *evidence)
{
    static const char *const allowed[] = {
        "manual_confirmed", "elapsed_seconds", "observations", "sensors",
        "tool_results"
    };
    const cJSON *manual;
    const cJSON *elapsed;
    const cJSON *observations;
    const cJSON *sensors;
    const cJSON *tools;

    if (!fields_allowed(evidence, allowed, 5)) {
        return false;
    }
    manual = cJSON_GetObjectItem(evidence, "manual_confirmed");
    elapsed = cJSON_GetObjectItem(evidence, "elapsed_seconds");
    observations = cJSON_GetObjectItem(evidence, "observations");
    sensors = cJSON_GetObjectItem(evidence, "sensors");
    tools = cJSON_GetObjectItem(evidence, "tool_results");
    return (!manual || cJSON_IsBool(manual)) &&
           (!elapsed || valid_integer(elapsed, 0, INT32_MAX)) &&
           (!observations || cJSON_IsObject(observations)) &&
           (!sensors || cJSON_IsObject(sensors)) &&
           (!tools || cJSON_IsObject(tools));
}

static int compare_value(const cJSON *actual, const cJSON *expected,
                         const char *operator_name)
{
    if (strcmp(operator_name, "eq") == 0) {
        return primitive_equal(actual, expected) ? 1 : 0;
    }
    if (strcmp(operator_name, "ne") == 0) {
        return primitive_equal(actual, expected) ? 0 : 1;
    }
    if (!cJSON_IsNumber(actual) || !cJSON_IsNumber(expected)) {
        return -EINVAL;
    }
    if (strcmp(operator_name, "gt") == 0) return actual->valuedouble > expected->valuedouble;
    if (strcmp(operator_name, "gte") == 0) return actual->valuedouble >= expected->valuedouble;
    if (strcmp(operator_name, "lt") == 0) return actual->valuedouble < expected->valuedouble;
    if (strcmp(operator_name, "lte") == 0) return actual->valuedouble <= expected->valuedouble;
    return -EINVAL;
}

static int evaluate_condition(const cJSON *condition, const cJSON *evidence,
                              char *reason, size_t reason_size)
{
    const cJSON *type = cJSON_GetObjectItem(condition, "type");

    if (strcmp(type->valuestring, "manual_confirm") == 0) {
        const cJSON *confirmed = cJSON_GetObjectItem(evidence, "manual_confirmed");
        if (cJSON_IsTrue(confirmed)) return 1;
        snprintf(reason, reason_size, "manual confirmation required");
        return 0;
    }
    if (strcmp(type->valuestring, "timer_elapsed") == 0) {
        const cJSON *actual = cJSON_GetObjectItem(evidence, "elapsed_seconds");
        const cJSON *required = cJSON_GetObjectItem(condition, "duration_seconds");
        if (valid_integer(actual, 0, INT32_MAX) &&
            actual->valueint >= required->valueint) return 1;
        snprintf(reason, reason_size, "timer has not elapsed");
        return 0;
    }
    if (strcmp(type->valuestring, "observation_required") == 0) {
        const cJSON *observations = cJSON_GetObjectItem(evidence, "observations");
        const cJSON *key = cJSON_GetObjectItem(condition, "key");
        const cJSON *value = cJSON_IsObject(observations) ?
            cJSON_GetObjectItem(observations, key->valuestring) : NULL;
        if (value && !cJSON_IsNull(value)) return 1;
        snprintf(reason, reason_size, "observation missing: %s", key->valuestring);
        return 0;
    }
    if (strcmp(type->valuestring, "sensor_range") == 0) {
        const cJSON *sensors = cJSON_GetObjectItem(evidence, "sensors");
        const cJSON *sensor_name = cJSON_GetObjectItem(condition, "sensor");
        const cJSON *sample = cJSON_IsObject(sensors) ?
            cJSON_GetObjectItem(sensors, sensor_name->valuestring) : NULL;
        const cJSON *value = cJSON_IsObject(sample) ? cJSON_GetObjectItem(sample, "value") : NULL;
        const cJSON *stable = cJSON_IsObject(sample) ? cJSON_GetObjectItem(sample, "stable_for_seconds") : NULL;
        const cJSON *age = cJSON_IsObject(sample) ? cJSON_GetObjectItem(sample, "age_ms") : NULL;
        const cJSON *unit = cJSON_IsObject(sample) ? cJSON_GetObjectItem(sample, "unit") : NULL;
        const cJSON *required_unit = cJSON_GetObjectItem(condition, "unit");
        const cJSON *required_stable = cJSON_GetObjectItem(condition, "stable_for_seconds");
        const cJSON *freshness = cJSON_GetObjectItem(condition, "freshness_ms");
        const cJSON *minimum = cJSON_GetObjectItem(condition, "minimum");
        const cJSON *maximum = cJSON_GetObjectItem(condition, "maximum");

        if (!cJSON_IsNumber(value) || value->valuedouble < minimum->valuedouble ||
            value->valuedouble > maximum->valuedouble ||
            (required_unit && (!cJSON_IsString(unit) ||
             strcmp(unit->valuestring, required_unit->valuestring) != 0)) ||
            (required_stable && (!valid_integer(stable, 0, INT32_MAX) ||
             stable->valueint < required_stable->valueint)) ||
            (freshness && (!valid_integer(age, 0, INT32_MAX) ||
             age->valueint > freshness->valueint))) {
            snprintf(reason, reason_size, "sensor condition unmet: %s",
                     sensor_name->valuestring);
            return 0;
        }
        return 1;
    }
    if (strcmp(type->valuestring, "tool_result") == 0) {
        const cJSON *tools = cJSON_GetObjectItem(evidence, "tool_results");
        const cJSON *tool = cJSON_GetObjectItem(condition, "tool");
        const cJSON *field = cJSON_GetObjectItem(condition, "field");
        const cJSON *operator_item = cJSON_GetObjectItem(condition, "operator");
        const cJSON *expected = cJSON_GetObjectItem(condition, "value");
        const cJSON *result = cJSON_IsObject(tools) ?
            cJSON_GetObjectItem(tools, tool->valuestring) : NULL;
        const cJSON *actual = cJSON_IsObject(result) ?
            cJSON_GetObjectItem(result, field->valuestring) : NULL;
        int comparison;

        if (!actual || (comparison = compare_value(actual, expected,
                                                    operator_item->valuestring)) <= 0) {
            snprintf(reason, reason_size, "tool result unmet: %s.%s",
                     tool->valuestring, field->valuestring);
            return actual && comparison < 0 ? -EINVAL : 0;
        }
        return 1;
    }
    snprintf(reason, reason_size, "unsupported condition");
    return -EINVAL;
}

static int evaluate_conditions(const cJSON *conditions, const cJSON *evidence,
                               char *reason, size_t reason_size)
{
    const cJSON *condition;

    if (!conditions) {
        return 1;
    }
    cJSON_ArrayForEach(condition, conditions) {
        int result = evaluate_condition(condition, evidence, reason, reason_size);
        if (result <= 0) {
            return result;
        }
    }
    snprintf(reason, reason_size, "conditions satisfied");
    return 1;
}

static void set_string(cJSON *object, const char *name, const char *value)
{
    cJSON_ReplaceItemInObject(object, name, cJSON_CreateString(value));
}

static void set_number(cJSON *object, const char *name, int value)
{
    cJSON_ReplaceItemInObject(object, name, cJSON_CreateNumber(value));
}

static void set_phase(cJSON *run, controller_phase_t phase)
{
    set_string(run, "phase", phase_name(phase));
}

static void set_reason(cJSON *run, const char *reason)
{
    set_string(run, "last_reason", reason ? reason : "");
}

static int replace_evidence(cJSON *run, const cJSON *evidence)
{
    cJSON *copy = cJSON_Duplicate(evidence, true);
    if (!copy) {
        return -ENOMEM;
    }
    cJSON_ReplaceItemInObject(run, "last_evidence", copy);
    return 0;
}

static int policy_apply(cJSON *run, const cJSON *step, const char *policy_field,
                        const char *cause)
{
    const cJSON *policy_item = cJSON_GetObjectItem(step, policy_field);
    const char *policy = cJSON_IsString(policy_item) ?
        policy_item->valuestring : "ask_operator";
    controller_phase_t phase = parse_phase(cJSON_GetObjectItem(run, "phase"));

    if (strcmp(policy, "retry") == 0) {
        const cJSON *limit = cJSON_GetObjectItem(step, "retry_limit");
        int attempt = cJSON_GetObjectItem(run, "attempt")->valueint;
        if (valid_integer(limit, 1, 3) && attempt < limit->valueint) {
            set_number(run, "attempt", attempt + 1);
            set_phase(run, PHASE_EXECUTE);
            set_reason(run, cause);
            return 0;
        }
        cJSON_ReplaceItemInObject(run, "resume_phase", cJSON_CreateString("EXECUTE"));
        set_phase(run, PHASE_NEEDS_HELP);
        set_reason(run, "retry limit reached");
        return 0;
    }
    if (strcmp(policy, "pause") == 0) {
        cJSON_ReplaceItemInObject(run, "resume_phase",
                                  cJSON_CreateString(phase_name(phase)));
        set_phase(run, PHASE_PAUSED);
        set_reason(run, cause);
        return 0;
    }
    if (strcmp(policy, "abort") == 0) {
        set_phase(run, PHASE_ABORTED);
        set_reason(run, cause);
        return 0;
    }
    cJSON_ReplaceItemInObject(run, "resume_phase",
                              cJSON_CreateString(phase_name(phase)));
    set_phase(run, PHASE_NEEDS_HELP);
    set_reason(run, cause);
    return 0;
}

int labtwin_controller_init(void)
{
    int result;

    pthread_mutex_lock(&g_controller_lock);
    if (g_controller_initialized) {
        pthread_mutex_unlock(&g_controller_lock);
        return 0;
    }
    result = mkdir_checked(LABTWIN_ROOT);
    if (result == 0) {
        result = mkdir_checked(CONTROLLER_DIR);
    }
    if (result == 0) {
        g_controller_initialized = true;
    }
    pthread_mutex_unlock(&g_controller_lock);
    return result;
}

int labtwin_controller_create_json(const char *input, char *output,
                                   size_t output_size, const char *source)
{
    static const char *const allowed[] = {
        "run_id", "experiment_id", "protocol_id", "protocol_version", "parameters"
    };
    cJSON *request;
    const cJSON *run_id;
    const cJSON *experiment_id;
    const cJSON *protocol_id;
    const cJSON *version;
    const cJSON *provided;
    cJSON *protocol = NULL;
    cJSON *parameters = NULL;
    cJSON *run = NULL;
    char reason[REASON_MAX];
    char snapshot_path[PATH_MAX_LOCAL];
    char event_path[PATH_MAX_LOCAL];
    int result;

    if (!input || strlen(input) >= LABTWIN_CONTROLLER_MAX_JSON_SIZE) {
        return fail_response(output, output_size, -EINVAL, "INVALID_ARGUMENT",
                             "controller request is missing or too large");
    }
    request = cJSON_Parse(input);
    if (!request || !fields_allowed(request, allowed, 5) ||
        !cJSON_IsString(run_id = cJSON_GetObjectItem(request, "run_id")) ||
        !valid_identifier(run_id->valuestring, RUN_ID_MAX) ||
        ((experiment_id = cJSON_GetObjectItem(request, "experiment_id")) &&
         (!cJSON_IsString(experiment_id) ||
          !valid_identifier(experiment_id->valuestring, EXPERIMENT_ID_MAX))) ||
        !cJSON_IsString(protocol_id = cJSON_GetObjectItem(request, "protocol_id")) ||
        !valid_identifier(protocol_id->valuestring, PROTOCOL_ID_MAX) ||
        !valid_integer(version = cJSON_GetObjectItem(request, "protocol_version"),
                       1, UINT16_MAX) ||
        ((provided = cJSON_GetObjectItem(request, "parameters")) &&
         !cJSON_IsObject(provided))) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -EINVAL, "INVALID_ARGUMENT",
                             "invalid controller create request");
    }
    protocol = load_protocol(protocol_id->valuestring,
                             (unsigned int)version->valueint);
    if (!protocol) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -ENOENT, "PROTOCOL_NOT_FOUND",
                             "protocol version not found");
    }
    if (!provided) {
        provided = cJSON_CreateObject();
    }
    parameters = resolve_parameters(cJSON_GetObjectItem(protocol, "parameters"),
                                    provided, reason, sizeof(reason));
    if (provided != cJSON_GetObjectItem(request, "parameters")) {
        cJSON_Delete((cJSON *)provided);
    }
    if (!parameters) {
        cJSON_Delete(protocol);
        cJSON_Delete(request);
        return fail_response(output, output_size, -EINVAL, "INVALID_PARAMETERS",
                             reason);
    }
    run = cJSON_CreateObject();
    if (!run) {
        cJSON_Delete(parameters);
        cJSON_Delete(protocol);
        cJSON_Delete(request);
        return fail_response(output, output_size, -ENOMEM, "NO_MEMORY",
                             "controller state allocation failed");
    }
    cJSON_AddNumberToObject(run, "schema_version", LABTWIN_CONTROLLER_SCHEMA_VERSION);
    cJSON_AddStringToObject(run, "run_id", run_id->valuestring);
    cJSON_AddStringToObject(run, "experiment_id",
                            experiment_id ? experiment_id->valuestring : "");
    cJSON_AddStringToObject(run, "protocol_id", protocol_id->valuestring);
    cJSON_AddNumberToObject(run, "protocol_version", version->valueint);
    cJSON_AddStringToObject(run, "phase", "PLAN");
    cJSON_AddStringToObject(run, "resume_phase", "PLAN");
    cJSON_AddStringToObject(run, "scope", "prerequisites");
    cJSON_AddNumberToObject(run, "current_step", 0);
    cJSON_AddNumberToObject(run, "step_count",
                            cJSON_GetArraySize(cJSON_GetObjectItem(protocol, "steps")));
    cJSON_AddNumberToObject(run, "attempt", 0);
    cJSON_AddNumberToObject(run, "last_seq", 0);
    cJSON_AddStringToObject(run, "last_reason", "awaiting plan evaluation");
    cJSON_AddItemToObject(run, "parameters", parameters);
    cJSON_AddItemToObject(run, "last_evidence", cJSON_CreateObject());

    result = labtwin_controller_init();
    if (result != 0) {
        cJSON_Delete(run);
        cJSON_Delete(protocol);
        cJSON_Delete(request);
        return fail_response(output, output_size, -EIO, "STORAGE_ERROR",
                             "controller store is unavailable");
    }
    if (experiment_id && labtwin_service_init() != 0) {
        cJSON_Delete(run);
        cJSON_Delete(protocol);
        cJSON_Delete(request);
        return fail_response(output, output_size, -EIO, "EXPERIMENT_SYNC_FAILED",
                             "linked experiment service is unavailable");
    }
    run_paths(run_id->valuestring, snapshot_path, sizeof(snapshot_path),
              event_path, sizeof(event_path));
    pthread_mutex_lock(&g_controller_lock);
    {
        cJSON *existing_snapshot = read_json_file(snapshot_path);
        cJSON *existing_event = recover_latest_event(event_path, 0);
        bool exists = existing_snapshot != NULL || existing_event != NULL;
        cJSON_Delete(existing_snapshot);
        cJSON_Delete(existing_event);
        if (exists) {
            pthread_mutex_unlock(&g_controller_lock);
            cJSON_Delete(run);
            cJSON_Delete(protocol);
            cJSON_Delete(request);
            return fail_response(output, output_size, -EEXIST, "RUN_EXISTS",
                                 "run_id already exists");
        }
    }
    /* Establish the linked experiment projection before exposing the run. */
    result = sync_create_experiment(run, protocol, source);
    if (result != 0) {
        pthread_mutex_unlock(&g_controller_lock);
        cJSON_Delete(run);
        cJSON_Delete(protocol);
        cJSON_Delete(request);
        return fail_response(output, output_size, result,
                             "EXPERIMENT_SYNC_FAILED",
                             "linked experiment is unavailable or incompatible");
    }
    result = commit_locked(run, "CREATED", source);
    pthread_mutex_unlock(&g_controller_lock);
    cJSON_Delete(protocol);
    cJSON_Delete(request);
    if (result != 0) {
        cJSON_Delete(run);
        return fail_response(output, output_size, result, "STORAGE_ERROR",
                             "controller run could not be persisted");
    }
    return emit_response(output, output_size, true, "OK",
                         "controller run created", run);
}

int labtwin_controller_get_json(const char *input, char *output,
                                size_t output_size)
{
    static const char *const allowed[] = {"run_id"};
    cJSON *request = cJSON_Parse(input ? input : "{}");
    const cJSON *run_id;
    cJSON *run;

    if (!request || !fields_allowed(request, allowed, 1) ||
        !cJSON_IsString(run_id = cJSON_GetObjectItem(request, "run_id")) ||
        !valid_identifier(run_id->valuestring, RUN_ID_MAX)) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -EINVAL, "INVALID_ARGUMENT",
                             "valid run_id is required");
    }
    if (labtwin_controller_init() != 0) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -EIO, "STORAGE_ERROR",
                             "controller store is unavailable");
    }
    pthread_mutex_lock(&g_controller_lock);
    run = load_run_locked(run_id->valuestring);
    pthread_mutex_unlock(&g_controller_lock);
    cJSON_Delete(request);
    if (!run) {
        return fail_response(output, output_size, -ENOENT, "NOT_FOUND",
                             "controller run not found");
    }
    return emit_response(output, output_size, true, "OK",
                         "controller run found", run);
}

int labtwin_controller_command_json(const char *input, char *output,
                                    size_t output_size, const char *source)
{
    static const char *const allowed[] = {
        "run_id", "expected_seq", "action", "evidence", "reason"
    };
    cJSON *request;
    const cJSON *run_id;
    const cJSON *expected_seq;
    const cJSON *action;
    const cJSON *evidence;
    const cJSON *requested_reason;
    cJSON *run = NULL;
    cJSON *protocol = NULL;
    const cJSON *steps;
    const cJSON *step = NULL;
    controller_phase_t phase;
    char reason[REASON_MAX] = "";
    const char *event_type = "COMMAND";
    int result = 0;

    if (!input || strlen(input) >= LABTWIN_CONTROLLER_MAX_JSON_SIZE) {
        return fail_response(output, output_size, -EINVAL, "INVALID_ARGUMENT",
                             "controller request is missing or too large");
    }
    request = cJSON_Parse(input);
    if (!request || !fields_allowed(request, allowed, 5) ||
        !cJSON_IsString(run_id = cJSON_GetObjectItem(request, "run_id")) ||
        !valid_identifier(run_id->valuestring, RUN_ID_MAX) ||
        !valid_integer(expected_seq = cJSON_GetObjectItem(request, "expected_seq"),
                       1, INT32_MAX) ||
        !cJSON_IsString(action = cJSON_GetObjectItem(request, "action")) ||
        strlen(action->valuestring) >= 24 ||
        ((evidence = cJSON_GetObjectItem(request, "evidence")) &&
         !valid_evidence(evidence)) ||
        ((requested_reason = cJSON_GetObjectItem(request, "reason")) &&
         (!cJSON_IsString(requested_reason) || strlen(requested_reason->valuestring) >= REASON_MAX))) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -EINVAL, "INVALID_ARGUMENT",
                             "invalid controller command");
    }
    if (labtwin_controller_init() != 0) {
        cJSON_Delete(request);
        return fail_response(output, output_size, -EIO, "STORAGE_ERROR",
                             "controller store is unavailable");
    }
    pthread_mutex_lock(&g_controller_lock);
    run = load_run_locked(run_id->valuestring);
    if (!run) {
        result = -ENOENT;
        goto done;
    }
    if (run_sequence(run) != (unsigned int)expected_seq->valueint) {
        result = -EAGAIN;
        goto done;
    }
    phase = parse_phase(cJSON_GetObjectItem(run, "phase"));
    protocol = load_protocol(cJSON_GetObjectItem(run, "protocol_id")->valuestring,
        (unsigned int)cJSON_GetObjectItem(run, "protocol_version")->valueint);
    if (!protocol) {
        result = -ESTALE;
        goto done;
    }
    steps = cJSON_GetObjectItem(protocol, "steps");
    if (cJSON_GetObjectItem(run, "current_step")->valueint < cJSON_GetArraySize(steps)) {
        step = cJSON_GetArrayItem(steps,
            cJSON_GetObjectItem(run, "current_step")->valueint);
    }

    if (strcmp(action->valuestring, "plan") == 0) {
        int evaluation;
        if (phase != PHASE_PLAN || !evidence) { result = -EPERM; goto done; }
        evaluation = evaluate_conditions(cJSON_GetObjectItem(protocol, "prerequisites"),
                                         evidence, reason, sizeof(reason));
        if (replace_evidence(run, evidence) != 0) {
            result = -ENOMEM;
            goto done;
        }
        if (evaluation > 0) {
            set_phase(run, PHASE_EXECUTE);
            set_string(run, "scope", "step");
        } else if (evaluation < 0) {
            set_string(run, "resume_phase", "PLAN");
            set_phase(run, PHASE_NEEDS_HELP);
        }
        set_reason(run, reason);
        event_type = evaluation > 0 ? "PLAN_ACCEPTED" : "PLAN_BLOCKED";
    } else if (strcmp(action->valuestring, "execute") == 0) {
        int evaluation;
        const cJSON *step_prerequisites;
        cJSON *empty = NULL;
        if (phase != PHASE_EXECUTE || !step) { result = -EPERM; goto done; }
        if (!evidence) {
            empty = cJSON_CreateObject();
            evidence = empty;
        }
        step_prerequisites = cJSON_GetObjectItem(step, "prerequisites");
        evaluation = evaluate_conditions(step_prerequisites, evidence,
                                         reason, sizeof(reason));
        if (replace_evidence(run, evidence) != 0) {
            cJSON_Delete(empty);
            result = -ENOMEM;
            goto done;
        }
        cJSON_Delete(empty);
        if (evaluation > 0) {
            set_phase(run, PHASE_OBSERVE);
            set_reason(run, "step execution acknowledged; observation required");
            event_type = "STEP_EXECUTED";
        } else if (evaluation == 0) {
            set_reason(run, reason);
            event_type = "STEP_PREREQUISITE_BLOCKED";
        } else {
            set_string(run, "resume_phase", "EXECUTE");
            set_phase(run, PHASE_NEEDS_HELP);
            set_reason(run, reason);
            event_type = "EVALUATION_ERROR";
        }
    } else if (strcmp(action->valuestring, "observe") == 0) {
        if (phase != PHASE_OBSERVE || !evidence) { result = -EPERM; goto done; }
        if (replace_evidence(run, evidence) != 0) { result = -ENOMEM; goto done; }
        set_phase(run, PHASE_EVALUATE);
        set_reason(run, "evidence recorded; evaluation required");
        event_type = "OBSERVED";
    } else if (strcmp(action->valuestring, "evaluate") == 0) {
        const cJSON *conditions;
        const cJSON *stored_evidence = cJSON_GetObjectItem(run, "last_evidence");
        bool acceptance = strcmp(cJSON_GetObjectItem(run, "scope")->valuestring,
                                 "acceptance") == 0;
        int evaluation;
        if (phase != PHASE_EVALUATE) { result = -EPERM; goto done; }
        conditions = acceptance ? cJSON_GetObjectItem(protocol, "acceptance_criteria") :
                                  cJSON_GetObjectItem(step, "completion_condition");
        evaluation = acceptance ? evaluate_conditions(conditions, stored_evidence,
                                                       reason, sizeof(reason)) :
                                  evaluate_condition(conditions, stored_evidence,
                                                     reason, sizeof(reason));
        if (evaluation < 0) {
            set_string(run, "resume_phase", "OBSERVE");
            set_phase(run, PHASE_NEEDS_HELP);
            set_reason(run, reason);
            event_type = "EVALUATION_ERROR";
        } else if (evaluation == 0) {
            set_phase(run, PHASE_OBSERVE);
            set_reason(run, reason);
            event_type = acceptance ? "ACCEPTANCE_BLOCKED" : "STEP_INCOMPLETE";
        } else if (acceptance) {
            set_phase(run, PHASE_COMPLETED);
            set_reason(run, "protocol acceptance criteria satisfied");
            event_type = "COMPLETED";
        } else {
            int next = cJSON_GetObjectItem(run, "current_step")->valueint + 1;
            set_number(run, "current_step", next);
            set_number(run, "attempt", 0);
            if (next >= cJSON_GetArraySize(steps)) {
                set_string(run, "scope", "acceptance");
                set_phase(run, PHASE_OBSERVE);
                set_reason(run, "all steps complete; acceptance evidence required");
                event_type = "STEPS_COMPLETED";
            } else {
                set_phase(run, PHASE_EXECUTE);
                set_reason(run, "step completed; next step ready");
                event_type = "STEP_COMPLETED";
            }
        }
    } else if (strcmp(action->valuestring, "pause") == 0) {
        if (!active_phase(phase)) { result = -EPERM; goto done; }
        set_string(run, "resume_phase", phase_name(phase));
        set_phase(run, PHASE_PAUSED);
        set_reason(run, requested_reason ? requested_reason->valuestring : "paused by request");
        event_type = "PAUSED";
    } else if (strcmp(action->valuestring, "request_help") == 0) {
        if (!active_phase(phase) && phase != PHASE_PAUSED) { result = -EPERM; goto done; }
        if (active_phase(phase)) set_string(run, "resume_phase", phase_name(phase));
        set_phase(run, PHASE_NEEDS_HELP);
        set_reason(run, requested_reason ? requested_reason->valuestring : "operator help requested");
        event_type = "HELP_REQUESTED";
    } else if (strcmp(action->valuestring, "resume") == 0) {
        controller_phase_t resume;
        if (phase != PHASE_PAUSED && phase != PHASE_NEEDS_HELP) { result = -EPERM; goto done; }
        resume = parse_phase(cJSON_GetObjectItem(run, "resume_phase"));
        if (!active_phase(resume)) { result = -EINVAL; goto done; }
        set_phase(run, resume);
        set_reason(run, "resumed");
        event_type = "RESUMED";
    } else if (strcmp(action->valuestring, "timeout") == 0 ||
               strcmp(action->valuestring, "failure") == 0) {
        if (!active_phase(phase) || phase == PHASE_PLAN || !step ||
            strcmp(cJSON_GetObjectItem(run, "scope")->valuestring, "step") != 0) {
            result = -EPERM;
            goto done;
        }
        policy_apply(run, step,
                     strcmp(action->valuestring, "timeout") == 0 ?
                     "on_timeout" : "on_failure",
                     requested_reason ? requested_reason->valuestring :
                     action->valuestring);
        event_type = strcmp(action->valuestring, "timeout") == 0 ?
                     "TIMEOUT_HANDLED" : "FAILURE_HANDLED";
    } else if (strcmp(action->valuestring, "abort") == 0) {
        if (phase == PHASE_COMPLETED || phase == PHASE_ABORTED) {
            result = -EPERM;
            goto done;
        }
        set_phase(run, PHASE_ABORTED);
        set_reason(run, requested_reason ? requested_reason->valuestring : "aborted by request");
        event_type = "ABORTED";
    } else {
        result = -EINVAL;
        goto done;
    }
    result = commit_locked(run, event_type, source);

done:
    pthread_mutex_unlock(&g_controller_lock);
    cJSON_Delete(protocol);
    cJSON_Delete(request);
    if (result == -ENOENT) {
        cJSON_Delete(run);
        return fail_response(output, output_size, -ENOENT, "NOT_FOUND",
                             "controller run not found");
    }
    if (result == -EAGAIN) {
        cJSON_Delete(run);
        return fail_response(output, output_size, -EAGAIN, "SEQUENCE_CONFLICT",
                             "expected_seq does not match");
    }
    if (result == -ESTALE) {
        cJSON_Delete(run);
        return fail_response(output, output_size, -ESTALE, "PROTOCOL_UNAVAILABLE",
                             "bound protocol version is unavailable");
    }
    if (result == -EPERM || result == -EINVAL) {
        cJSON_Delete(run);
        return fail_response(output, output_size, result, "INVALID_TRANSITION",
                             "action is not valid in the current state");
    }
    if (result != 0) {
        cJSON_Delete(run);
        return fail_response(output, output_size, result, "STORAGE_ERROR",
                             "controller state could not be persisted");
    }
    return emit_response(output, output_size, true, "OK",
                         "controller command applied", run);
}
