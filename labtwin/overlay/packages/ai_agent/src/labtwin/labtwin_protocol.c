#include "labtwin/labtwin_protocol.h"

#ifdef LABTWIN_PROTOCOL_HOST_TEST
#include <syslog.h>
#else
#include "agent_compat.h"
#endif
#include "cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LABTWIN_ROOT
#ifdef LABTWIN_PROTOCOL_HOST_TEST
#define LABTWIN_ROOT "/tmp/labtwin-protocol-v1-test"
#else
#define LABTWIN_ROOT "/data/labtwin"
#endif
#endif

#define LABTWIN_PROTOCOL_DIR LABTWIN_ROOT "/protocols"
#define LABTWIN_PROTOCOL_ID_MAX 32
#define LABTWIN_PROTOCOL_NAME_MAX 64
#define LABTWIN_PROTOCOL_DESCRIPTION_MAX 513
#define LABTWIN_PROTOCOL_STEP_ID_MAX 32
#define LABTWIN_PROTOCOL_STEP_TITLE_MAX 96
#define LABTWIN_PROTOCOL_INSTRUCTIONS_MAX 513
#define LABTWIN_PROTOCOL_CONDITION_TEXT_MAX 96
#define LABTWIN_PROTOCOL_MAX_TIMEOUT_SECONDS (7 * 24 * 60 * 60)
#define LABTWIN_PROTOCOL_MAX_STABLE_SECONDS (60 * 60)
#define LABTWIN_PROTOCOL_MAX_FRESHNESS_MS (10 * 60 * 1000)

typedef struct {
    char protocol_id[LABTWIN_PROTOCOL_ID_MAX];
    char name[LABTWIN_PROTOCOL_NAME_MAX];
    unsigned int version;
} protocol_summary_t;

static pthread_mutex_t g_protocol_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_protocol_initialized;

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

static bool valid_string(const cJSON *item, size_t max_len, bool allow_empty)
{
    size_t length;

    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    length = strlen(item->valuestring);
    return (allow_empty || length > 0) && length < max_len;
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

static bool string_in_set(const cJSON *item, const char *const *values,
                          size_t count)
{
    if (!cJSON_IsString(item)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(item->valuestring, values[i]) == 0) {
            return true;
        }
    }
    return false;
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
        snprintf(output, output_size,
                 "{\"ok\":false,\"code\":\"NO_MEMORY\"}");
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
        snprintf(output, output_size,
                 "{\"ok\":false,\"code\":\"NO_MEMORY\"}");
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

static int validation_error(char *error, size_t error_size,
                            const char *field, const char *reason)
{
    snprintf(error, error_size, "%s: %s", field, reason);
    return -EINVAL;
}

static bool parameter_value_matches(const cJSON *value, const char *type)
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

static int validate_parameter(const cJSON *parameter, char *error,
                              size_t error_size)
{
    static const char *const allowed[] = {
        "name", "type", "unit", "required", "default", "minimum",
        "maximum", "enum"
    };
    static const char *const types[] = {
        "string", "integer", "number", "boolean"
    };
    const cJSON *name;
    const cJSON *type;
    const cJSON *unit;
    const cJSON *required;
    const cJSON *default_value;
    const cJSON *minimum;
    const cJSON *maximum;
    const cJSON *enum_values;
    const cJSON *enum_value;

    if (!fields_allowed(parameter, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        return validation_error(error, error_size, "parameters", "unknown field");
    }
    name = cJSON_GetObjectItem(parameter, "name");
    type = cJSON_GetObjectItem(parameter, "type");
    unit = cJSON_GetObjectItem(parameter, "unit");
    required = cJSON_GetObjectItem(parameter, "required");
    default_value = cJSON_GetObjectItem(parameter, "default");
    minimum = cJSON_GetObjectItem(parameter, "minimum");
    maximum = cJSON_GetObjectItem(parameter, "maximum");
    enum_values = cJSON_GetObjectItem(parameter, "enum");

    if (!cJSON_IsString(name) ||
        !valid_identifier(name->valuestring, LABTWIN_PROTOCOL_STEP_ID_MAX)) {
        return validation_error(error, error_size, "parameters.name", "invalid identifier");
    }
    if (!string_in_set(type, types, sizeof(types) / sizeof(types[0]))) {
        return validation_error(error, error_size, "parameters.type", "unsupported type");
    }
    if (unit && !valid_string(unit, 24, false)) {
        return validation_error(error, error_size, "parameters.unit", "invalid unit");
    }
    if (required && !cJSON_IsBool(required)) {
        return validation_error(error, error_size, "parameters.required", "must be boolean");
    }
    if (default_value &&
        !parameter_value_matches(default_value, type->valuestring)) {
        return validation_error(error, error_size, "parameters.default", "type mismatch");
    }

    if (minimum || maximum) {
        if ((strcmp(type->valuestring, "integer") != 0 &&
             strcmp(type->valuestring, "number") != 0) ||
            (minimum && !cJSON_IsNumber(minimum)) ||
            (maximum && !cJSON_IsNumber(maximum)) ||
            (minimum && maximum &&
             minimum->valuedouble > maximum->valuedouble)) {
            return validation_error(error, error_size, "parameters.range", "invalid range");
        }
    }

    if (enum_values) {
        int count;
        if (!cJSON_IsArray(enum_values) ||
            (count = cJSON_GetArraySize(enum_values)) < 1 || count > 16) {
            return validation_error(error, error_size, "parameters.enum", "invalid enum");
        }
        cJSON_ArrayForEach(enum_value, enum_values) {
            if (!parameter_value_matches(enum_value, type->valuestring)) {
                return validation_error(error, error_size, "parameters.enum", "type mismatch");
            }
        }
    }
    return 0;
}

static int validate_condition(const cJSON *condition, char *error,
                              size_t error_size)
{
    static const char *const manual_fields[] = {"type", "prompt"};
    static const char *const timer_fields[] = {"type", "duration_seconds"};
    static const char *const observation_fields[] = {"type", "key"};
    static const char *const sensor_fields[] = {
        "type", "sensor", "unit", "minimum", "maximum",
        "stable_for_seconds", "freshness_ms"
    };
    static const char *const tool_fields[] = {
        "type", "tool", "field", "operator", "value"
    };
    static const char *const operators[] = {"eq", "ne", "gt", "gte", "lt", "lte"};
    const cJSON *type;

    if (!cJSON_IsObject(condition) ||
        !valid_string(type = cJSON_GetObjectItem(condition, "type"), 32, false)) {
        return validation_error(error, error_size, "condition.type", "required");
    }

    if (strcmp(type->valuestring, "manual_confirm") == 0) {
        const cJSON *prompt = cJSON_GetObjectItem(condition, "prompt");
        if (!fields_allowed(condition, manual_fields, 2) ||
            (prompt && !valid_string(prompt, LABTWIN_PROTOCOL_CONDITION_TEXT_MAX, false))) {
            return validation_error(error, error_size, "manual_confirm", "invalid fields");
        }
        return 0;
    }

    if (strcmp(type->valuestring, "timer_elapsed") == 0) {
        if (!fields_allowed(condition, timer_fields, 2) ||
            !valid_integer(cJSON_GetObjectItem(condition, "duration_seconds"),
                           1, LABTWIN_PROTOCOL_MAX_TIMEOUT_SECONDS)) {
            return validation_error(error, error_size, "timer_elapsed", "invalid duration");
        }
        return 0;
    }

    if (strcmp(type->valuestring, "observation_required") == 0) {
        const cJSON *key = cJSON_GetObjectItem(condition, "key");
        if (!fields_allowed(condition, observation_fields, 2) ||
            !valid_string(key, LABTWIN_PROTOCOL_CONDITION_TEXT_MAX, false)) {
            return validation_error(error, error_size, "observation_required", "invalid key");
        }
        return 0;
    }

    if (strcmp(type->valuestring, "sensor_range") == 0) {
        const cJSON *sensor = cJSON_GetObjectItem(condition, "sensor");
        const cJSON *unit = cJSON_GetObjectItem(condition, "unit");
        const cJSON *minimum = cJSON_GetObjectItem(condition, "minimum");
        const cJSON *maximum = cJSON_GetObjectItem(condition, "maximum");
        const cJSON *stable = cJSON_GetObjectItem(condition, "stable_for_seconds");
        const cJSON *freshness = cJSON_GetObjectItem(condition, "freshness_ms");
        if (!fields_allowed(condition, sensor_fields, 7) ||
            !valid_string(sensor, 32, false) ||
            (unit && !valid_string(unit, 24, false)) ||
            !cJSON_IsNumber(minimum) || !cJSON_IsNumber(maximum) ||
            minimum->valuedouble > maximum->valuedouble ||
            (stable && !valid_integer(stable, 0, LABTWIN_PROTOCOL_MAX_STABLE_SECONDS)) ||
            (freshness && !valid_integer(freshness, 1, LABTWIN_PROTOCOL_MAX_FRESHNESS_MS))) {
            return validation_error(error, error_size, "sensor_range", "invalid fields or range");
        }
        return 0;
    }

    if (strcmp(type->valuestring, "tool_result") == 0) {
        const cJSON *tool = cJSON_GetObjectItem(condition, "tool");
        const cJSON *field = cJSON_GetObjectItem(condition, "field");
        const cJSON *op = cJSON_GetObjectItem(condition, "operator");
        const cJSON *value = cJSON_GetObjectItem(condition, "value");
        bool primitive = cJSON_IsString(value) || cJSON_IsNumber(value) ||
                         cJSON_IsBool(value) || cJSON_IsNull(value);
        if (!fields_allowed(condition, tool_fields, 5) ||
            !valid_string(tool, 48, false) ||
            !valid_string(field, LABTWIN_PROTOCOL_CONDITION_TEXT_MAX, false) ||
            !string_in_set(op, operators, sizeof(operators) / sizeof(operators[0])) ||
            !primitive) {
            return validation_error(error, error_size, "tool_result", "invalid fields");
        }
        return 0;
    }

    return validation_error(error, error_size, "condition.type", "unsupported condition");
}

static int validate_condition_array(const cJSON *conditions, int maximum,
                                    const char *field, char *error,
                                    size_t error_size, bool required)
{
    const cJSON *condition;
    int count;

    if (!conditions) {
        return required ? validation_error(error, error_size, field, "required") : 0;
    }
    if (!cJSON_IsArray(conditions) ||
        (count = cJSON_GetArraySize(conditions)) > maximum ||
        (required && count < 1)) {
        return validation_error(error, error_size, field, "invalid condition count");
    }
    cJSON_ArrayForEach(condition, conditions) {
        if (validate_condition(condition, error, error_size) != 0) {
            return -EINVAL;
        }
    }
    return 0;
}

static int validate_step(const cJSON *step, char *error, size_t error_size)
{
    static const char *const allowed[] = {
        "step_id", "title", "instructions", "prerequisites",
        "completion_condition", "timeout_seconds", "on_timeout",
        "on_failure", "retry_limit", "risk_level"
    };
    static const char *const failure_actions[] = {
        "pause", "ask_operator", "retry", "abort"
    };
    static const char *const risks[] = {"low", "medium", "high"};
    const cJSON *step_id;
    const cJSON *title;
    const cJSON *instructions;
    const cJSON *prerequisites;
    const cJSON *completion;
    const cJSON *timeout;
    const cJSON *on_timeout;
    const cJSON *on_failure;
    const cJSON *retry_limit;
    const cJSON *risk;

    if (!fields_allowed(step, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        return validation_error(error, error_size, "steps", "unknown field");
    }
    step_id = cJSON_GetObjectItem(step, "step_id");
    title = cJSON_GetObjectItem(step, "title");
    instructions = cJSON_GetObjectItem(step, "instructions");
    prerequisites = cJSON_GetObjectItem(step, "prerequisites");
    completion = cJSON_GetObjectItem(step, "completion_condition");
    timeout = cJSON_GetObjectItem(step, "timeout_seconds");
    on_timeout = cJSON_GetObjectItem(step, "on_timeout");
    on_failure = cJSON_GetObjectItem(step, "on_failure");
    retry_limit = cJSON_GetObjectItem(step, "retry_limit");
    risk = cJSON_GetObjectItem(step, "risk_level");

    if (!cJSON_IsString(step_id) ||
        !valid_identifier(step_id->valuestring, LABTWIN_PROTOCOL_STEP_ID_MAX)) {
        return validation_error(error, error_size, "steps.step_id", "invalid identifier");
    }
    if (!valid_string(title, LABTWIN_PROTOCOL_STEP_TITLE_MAX, false) ||
        (instructions && !valid_string(instructions, LABTWIN_PROTOCOL_INSTRUCTIONS_MAX, false))) {
        return validation_error(error, error_size, "steps", "invalid title or instructions");
    }
    if (validate_condition_array(prerequisites,
                                 LABTWIN_PROTOCOL_MAX_PREREQUISITES,
                                 "steps.prerequisites", error, error_size,
                                 false) != 0 ||
        validate_condition(completion, error, error_size) != 0) {
        return -EINVAL;
    }
    if (timeout && !valid_integer(timeout, 0,
                                  LABTWIN_PROTOCOL_MAX_TIMEOUT_SECONDS)) {
        return validation_error(error, error_size, "steps.timeout_seconds", "invalid timeout");
    }
    if (on_timeout && (!timeout || timeout->valueint < 1)) {
        return validation_error(error, error_size, "steps.timeout_seconds",
                                "positive timeout required with on_timeout");
    }
    if ((on_timeout && !string_in_set(on_timeout, failure_actions, 4)) ||
        (on_failure && !string_in_set(on_failure, failure_actions, 4)) ||
        (risk && !string_in_set(risk, risks, 3)) ||
        (retry_limit && !valid_integer(retry_limit, 0, 3))) {
        return validation_error(error, error_size, "steps", "invalid policy");
    }
    if (((on_timeout && strcmp(on_timeout->valuestring, "retry") == 0) ||
         (on_failure && strcmp(on_failure->valuestring, "retry") == 0)) &&
        (!retry_limit || retry_limit->valueint < 1)) {
        return validation_error(error, error_size, "steps.retry_limit", "required for retry");
    }
    return 0;
}

static int validate_protocol_root(const cJSON *root, char *error,
                                  size_t error_size)
{
    static const char *const allowed[] = {
        "schema_version", "protocol_id", "version", "name", "description",
        "parameters", "prerequisites", "steps", "acceptance_criteria"
    };
    const cJSON *schema_version;
    const cJSON *protocol_id;
    const cJSON *version;
    const cJSON *name;
    const cJSON *description;
    const cJSON *parameters;
    const cJSON *prerequisites;
    const cJSON *steps;
    const cJSON *acceptance;
    int parameter_count;
    int step_count;

    if (!fields_allowed(root, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        return validation_error(error, error_size, "protocol", "unknown field");
    }
    schema_version = cJSON_GetObjectItem(root, "schema_version");
    protocol_id = cJSON_GetObjectItem(root, "protocol_id");
    version = cJSON_GetObjectItem(root, "version");
    name = cJSON_GetObjectItem(root, "name");
    description = cJSON_GetObjectItem(root, "description");
    parameters = cJSON_GetObjectItem(root, "parameters");
    prerequisites = cJSON_GetObjectItem(root, "prerequisites");
    steps = cJSON_GetObjectItem(root, "steps");
    acceptance = cJSON_GetObjectItem(root, "acceptance_criteria");

    if (!valid_integer(schema_version, LABTWIN_PROTOCOL_SCHEMA_VERSION,
                       LABTWIN_PROTOCOL_SCHEMA_VERSION)) {
        return validation_error(error, error_size, "schema_version", "must be 1");
    }
    if (!cJSON_IsString(protocol_id) ||
        !valid_identifier(protocol_id->valuestring, LABTWIN_PROTOCOL_ID_MAX)) {
        return validation_error(error, error_size, "protocol_id", "invalid identifier");
    }
    if (!valid_integer(version, 1, UINT16_MAX)) {
        return validation_error(error, error_size, "version", "invalid version");
    }
    if (!valid_string(name, LABTWIN_PROTOCOL_NAME_MAX, false) ||
        (description && !valid_string(description,
                                      LABTWIN_PROTOCOL_DESCRIPTION_MAX,
                                      true))) {
        return validation_error(error, error_size, "name", "invalid name or description");
    }
    if (!cJSON_IsArray(parameters) ||
        (parameter_count = cJSON_GetArraySize(parameters)) >
            LABTWIN_PROTOCOL_MAX_PARAMETERS) {
        return validation_error(error, error_size, "parameters", "invalid parameter count");
    }
    for (int i = 0; i < parameter_count; i++) {
        const cJSON *candidate = cJSON_GetArrayItem(parameters, i);
        if (validate_parameter(candidate, error, error_size) != 0) {
            return -EINVAL;
        }
        for (int j = 0; j < i; j++) {
            const cJSON *previous = cJSON_GetArrayItem(parameters, j);
            if (strcmp(cJSON_GetObjectItem(candidate, "name")->valuestring,
                       cJSON_GetObjectItem(previous, "name")->valuestring) == 0) {
                return validation_error(error, error_size, "parameters.name", "duplicate");
            }
        }
    }
    if (validate_condition_array(prerequisites,
                                 LABTWIN_PROTOCOL_MAX_PREREQUISITES,
                                 "prerequisites", error, error_size,
                                 false) != 0) {
        return -EINVAL;
    }
    if (!cJSON_IsArray(steps) ||
        (step_count = cJSON_GetArraySize(steps)) < 1 ||
        step_count > LABTWIN_PROTOCOL_MAX_STEPS) {
        return validation_error(error, error_size, "steps", "invalid step count");
    }
    for (int i = 0; i < step_count; i++) {
        const cJSON *candidate = cJSON_GetArrayItem(steps, i);
        if (validate_step(candidate, error, error_size) != 0) {
            return -EINVAL;
        }
        for (int j = 0; j < i; j++) {
            const cJSON *previous = cJSON_GetArrayItem(steps, j);
            if (strcmp(cJSON_GetObjectItem(candidate, "step_id")->valuestring,
                       cJSON_GetObjectItem(previous, "step_id")->valuestring) == 0) {
                return validation_error(error, error_size, "steps.step_id", "duplicate");
            }
        }
    }
    if (validate_condition_array(acceptance,
                                 LABTWIN_PROTOCOL_MAX_ACCEPTANCE_CRITERIA,
                                 "acceptance_criteria", error, error_size,
                                 true) != 0) {
        return -EINVAL;
    }
    return 0;
}

static int parse_and_validate(const char *input, cJSON **root, char *error,
                              size_t error_size)
{
    if (!input || strlen(input) == 0 ||
        strlen(input) > LABTWIN_PROTOCOL_MAX_JSON_SIZE) {
        return validation_error(error, error_size, "protocol", "invalid size");
    }
    *root = cJSON_Parse(input);
    if (!*root) {
        return validation_error(error, error_size, "protocol", "invalid JSON");
    }
    if (validate_protocol_root(*root, error, error_size) != 0) {
        cJSON_Delete(*root);
        *root = NULL;
        return -EINVAL;
    }
    return 0;
}

static void protocol_path(const char *protocol_id, unsigned int version,
                          char *path, size_t path_size)
{
    snprintf(path, path_size, "%s/%s.v%u.json", LABTWIN_PROTOCOL_DIR,
             protocol_id, version);
}

static bool parse_protocol_filename(const char *filename, char *protocol_id,
                                    size_t protocol_id_size,
                                    unsigned int *version)
{
    char parsed_id[LABTWIN_PROTOCOL_ID_MAX];
    char expected[96];
    unsigned int parsed_version;

    if (sscanf(filename, "%31[^.].v%u.json", parsed_id, &parsed_version) != 2 ||
        !valid_identifier(parsed_id, sizeof(parsed_id)) || parsed_version == 0) {
        return false;
    }
    snprintf(expected, sizeof(expected), "%s.v%u.json", parsed_id,
             parsed_version);
    if (strcmp(filename, expected) != 0) {
        return false;
    }
    snprintf(protocol_id, protocol_id_size, "%s", parsed_id);
    *version = parsed_version;
    return true;
}

static unsigned int latest_version_locked(const char *protocol_id,
                                          size_t *stored_count)
{
    DIR *directory = opendir(LABTWIN_PROTOCOL_DIR);
    struct dirent *entry;
    unsigned int latest = 0;
    size_t count = 0;

    if (!directory) {
        if (stored_count) {
            *stored_count = 0;
        }
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char candidate[LABTWIN_PROTOCOL_ID_MAX];
        unsigned int version;
        if (!parse_protocol_filename(entry->d_name, candidate,
                                     sizeof(candidate), &version)) {
            continue;
        }
        count++;
        if (strcmp(candidate, protocol_id) == 0 && version > latest) {
            latest = version;
        }
    }
    closedir(directory);
    if (stored_count) {
        *stored_count = count;
    }
    return latest;
}

static int write_protocol_locked(const char *path, const cJSON *root)
{
    char temporary[192];
    char *serialized;
    FILE *file;
    int failed = 0;

    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    serialized = cJSON_PrintUnformatted(root);
    if (!serialized) {
        return -ENOMEM;
    }
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
        int error = errno;
        remove(temporary);
        return -error;
    }
    return 0;
}

static cJSON *read_protocol_document(const char *path)
{
    struct stat status;
    FILE *file;
    char *buffer;
    size_t bytes;
    cJSON *root;
    char error[128];

    if (stat(path, &status) != 0 || status.st_size <= 0 ||
        status.st_size > LABTWIN_PROTOCOL_MAX_JSON_SIZE + 1) {
        return NULL;
    }
    file = fopen(path, "r");
    if (!file) {
        return NULL;
    }
    buffer = malloc((size_t)status.st_size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    bytes = fread(buffer, 1, (size_t)status.st_size, file);
    fclose(file);
    buffer[bytes] = '\0';
    root = bytes == (size_t)status.st_size ? cJSON_Parse(buffer) : NULL;
    free(buffer);
    if (root && validate_protocol_root(root, error, sizeof(error)) != 0) {
        cJSON_Delete(root);
        root = NULL;
    }
    return root;
}

static int summary_compare(const void *left, const void *right)
{
    const protocol_summary_t *a = left;
    const protocol_summary_t *b = right;
    int by_id = strcmp(a->protocol_id, b->protocol_id);
    if (by_id != 0) {
        return by_id;
    }
    if (a->version < b->version) {
        return -1;
    }
    return a->version > b->version;
}

int labtwin_protocol_init(void)
{
    int result;

    pthread_mutex_lock(&g_protocol_lock);
    if (g_protocol_initialized) {
        pthread_mutex_unlock(&g_protocol_lock);
        return 0;
    }
    result = mkdir_checked(LABTWIN_ROOT);
    if (result == 0) {
        result = mkdir_checked(LABTWIN_PROTOCOL_DIR);
    }
    if (result == 0) {
        g_protocol_initialized = true;
    }
    pthread_mutex_unlock(&g_protocol_lock);
    return result;
}

int labtwin_protocol_validate_json(const char *input, char *output,
                                   size_t output_size)
{
    cJSON *root = NULL;
    cJSON *data;
    char error[128];

    if (parse_and_validate(input, &root, error, sizeof(error)) != 0) {
        emit_response(output, output_size, false, "INVALID_PROTOCOL", error,
                      NULL);
        return -EINVAL;
    }
    data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "schema_version",
                           LABTWIN_PROTOCOL_SCHEMA_VERSION);
    cJSON_AddStringToObject(data, "protocol_id",
                           cJSON_GetObjectItem(root, "protocol_id")->valuestring);
    cJSON_AddNumberToObject(data, "version",
                           cJSON_GetObjectItem(root, "version")->valueint);
    cJSON_Delete(root);
    return emit_response(output, output_size, true, "OK",
                         "protocol is valid", data);
}

int labtwin_protocol_create_json(const char *input, char *output,
                                 size_t output_size, const char *source)
{
    cJSON *root = NULL;
    const cJSON *id;
    const cJSON *version;
    char path[160];
    char error[128];
    unsigned int latest;
    size_t stored_count;
    int result;

    (void)source;
    if (labtwin_protocol_init() != 0) {
        emit_response(output, output_size, false, "STORAGE_ERROR",
                      "protocol store is unavailable", NULL);
        return -EIO;
    }
    if (parse_and_validate(input, &root, error, sizeof(error)) != 0) {
        emit_response(output, output_size, false, "INVALID_PROTOCOL", error,
                      NULL);
        return -EINVAL;
    }
    id = cJSON_GetObjectItem(root, "protocol_id");
    version = cJSON_GetObjectItem(root, "version");

    pthread_mutex_lock(&g_protocol_lock);
    latest = latest_version_locked(id->valuestring, &stored_count);
    if (stored_count >= LABTWIN_PROTOCOL_MAX_STORED_VERSIONS) {
        pthread_mutex_unlock(&g_protocol_lock);
        cJSON_Delete(root);
        emit_response(output, output_size, false, "LIMIT_REACHED",
                      "protocol store version limit reached", NULL);
        return -ENOSPC;
    }
    if ((latest == 0 && version->valueint != 1) ||
        (latest > 0 && (unsigned int)version->valueint != latest + 1)) {
        pthread_mutex_unlock(&g_protocol_lock);
        cJSON_Delete(root);
        emit_response(output, output_size, false, "VERSION_CONFLICT",
                      "version must start at 1 and increase by one", NULL);
        return -EEXIST;
    }
    protocol_path(id->valuestring, (unsigned int)version->valueint, path,
                  sizeof(path));
    result = write_protocol_locked(path, root);
    pthread_mutex_unlock(&g_protocol_lock);
    if (result != 0) {
        cJSON_Delete(root);
        emit_response(output, output_size, false, "STORAGE_ERROR",
                      "protocol could not be persisted", NULL);
        return result;
    }
    return emit_response(output, output_size, true, "OK",
                         "protocol version created", root);
}

int labtwin_protocol_get_json(const char *input, char *output,
                              size_t output_size)
{
    static const char *const allowed[] = {"protocol_id", "version"};
    cJSON *request = cJSON_Parse(input ? input : "{}");
    const cJSON *id;
    const cJSON *version;
    cJSON *document;
    char path[160];

    if (!request || !fields_allowed(request, allowed, 2) ||
        !cJSON_IsString(id = cJSON_GetObjectItem(request, "protocol_id")) ||
        !valid_identifier(id->valuestring, LABTWIN_PROTOCOL_ID_MAX) ||
        !valid_integer(version = cJSON_GetObjectItem(request, "version"),
                       1, UINT16_MAX)) {
        cJSON_Delete(request);
        emit_response(output, output_size, false, "INVALID_ARGUMENT",
                      "protocol_id and version are required", NULL);
        return -EINVAL;
    }
    if (labtwin_protocol_init() != 0) {
        cJSON_Delete(request);
        emit_response(output, output_size, false, "STORAGE_ERROR",
                      "protocol store is unavailable", NULL);
        return -EIO;
    }
    protocol_path(id->valuestring, (unsigned int)version->valueint, path,
                  sizeof(path));
    pthread_mutex_lock(&g_protocol_lock);
    document = read_protocol_document(path);
    pthread_mutex_unlock(&g_protocol_lock);
    cJSON_Delete(request);
    if (!document) {
        emit_response(output, output_size, false, "NOT_FOUND",
                      "protocol version not found", NULL);
        return -ENOENT;
    }
    return emit_response(output, output_size, true, "OK",
                         "protocol version found", document);
}

int labtwin_protocol_list_json(const char *input, char *output,
                               size_t output_size)
{
    static const char *const allowed[] = {"protocol_id", "offset", "limit"};
    cJSON *request = cJSON_Parse(input ? input : "{}");
    const cJSON *filter;
    const cJSON *offset_item;
    const cJSON *limit_item;
    protocol_summary_t summaries[LABTWIN_PROTOCOL_MAX_STORED_VERSIONS];
    size_t summary_count = 0;
    size_t offset = 0;
    size_t limit = 32;
    DIR *directory;
    struct dirent *entry;
    cJSON *data;
    cJSON *items;

    if (!request || !fields_allowed(request, allowed, 3)) {
        cJSON_Delete(request);
        emit_response(output, output_size, false, "INVALID_ARGUMENT",
                      "invalid list request", NULL);
        return -EINVAL;
    }
    filter = cJSON_GetObjectItem(request, "protocol_id");
    offset_item = cJSON_GetObjectItem(request, "offset");
    limit_item = cJSON_GetObjectItem(request, "limit");
    if ((filter && (!cJSON_IsString(filter) ||
                    !valid_identifier(filter->valuestring,
                                      LABTWIN_PROTOCOL_ID_MAX))) ||
        (offset_item && !valid_integer(offset_item, 0,
                                       LABTWIN_PROTOCOL_MAX_STORED_VERSIONS)) ||
        (limit_item && !valid_integer(limit_item, 1,
                                      LABTWIN_PROTOCOL_MAX_STORED_VERSIONS))) {
        cJSON_Delete(request);
        emit_response(output, output_size, false, "INVALID_ARGUMENT",
                      "invalid list filter or pagination", NULL);
        return -EINVAL;
    }
    if (offset_item) {
        offset = (size_t)offset_item->valueint;
    }
    if (limit_item) {
        limit = (size_t)limit_item->valueint;
    }
    if (labtwin_protocol_init() != 0) {
        cJSON_Delete(request);
        emit_response(output, output_size, false, "STORAGE_ERROR",
                      "protocol store is unavailable", NULL);
        return -EIO;
    }

    pthread_mutex_lock(&g_protocol_lock);
    directory = opendir(LABTWIN_PROTOCOL_DIR);
    if (directory) {
        while ((entry = readdir(directory)) != NULL &&
               summary_count < LABTWIN_PROTOCOL_MAX_STORED_VERSIONS) {
            char id[LABTWIN_PROTOCOL_ID_MAX];
            char path[160];
            unsigned int version;
            cJSON *document;
            const cJSON *name;

            if (!parse_protocol_filename(entry->d_name, id, sizeof(id),
                                         &version) ||
                (filter && strcmp(filter->valuestring, id) != 0)) {
                continue;
            }
            protocol_path(id, version, path, sizeof(path));
            document = read_protocol_document(path);
            name = document ? cJSON_GetObjectItem(document, "name") : NULL;
            if (valid_string(name, LABTWIN_PROTOCOL_NAME_MAX, false)) {
                snprintf(summaries[summary_count].protocol_id,
                         sizeof(summaries[summary_count].protocol_id), "%s", id);
                snprintf(summaries[summary_count].name,
                         sizeof(summaries[summary_count].name), "%s",
                         name->valuestring);
                summaries[summary_count].version = version;
                summary_count++;
            }
            cJSON_Delete(document);
        }
        closedir(directory);
    }
    pthread_mutex_unlock(&g_protocol_lock);
    cJSON_Delete(request);

    qsort(summaries, summary_count, sizeof(summaries[0]), summary_compare);
    data = cJSON_CreateObject();
    items = cJSON_AddArrayToObject(data, "items");
    for (size_t i = offset; i < summary_count && i < offset + limit; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "protocol_id", summaries[i].protocol_id);
        cJSON_AddNumberToObject(item, "version", summaries[i].version);
        cJSON_AddStringToObject(item, "name", summaries[i].name);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddNumberToObject(data, "count", (double)summary_count);
    cJSON_AddNumberToObject(data, "offset", (double)offset);
    cJSON_AddNumberToObject(data, "limit", (double)limit);
    return emit_response(output, output_size, true, "OK",
                         "protocol versions listed", data);
}
