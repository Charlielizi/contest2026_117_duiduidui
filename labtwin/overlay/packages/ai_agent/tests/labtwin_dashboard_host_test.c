#include "labtwin/labtwin.h"
#include "labtwin/labtwin_dashboard.h"

#include "cJSON.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TOKEN_PATH "/tmp/labtwin-dashboard-token"
#define TEST_TOKEN "m5-dashboard-test-token-2026"

bool labtwin_clock_trusted(void)
{
    return true;
}

int labtwin_experiment_snapshot_json(char *out, size_t out_size)
{
    const char *json =
        "{\"ok\":true,\"data\":{\"experiments\":[{"
        "\"schema_version\":2,\"experiment_id\":\"exp-1\","
        "\"name\":\"Host test\",\"state\":\"RUNNING\","
        "\"current_step\":0,\"recovery_reason\":\"\","
        "\"last_observation\":\"stable\",\"last_event_seq\":3,"
        "\"updated_epoch\":1770000000,"
        "\"steps\":[{\"title\":\"Prepare\",\"completed\":true}],"
        "\"timers\":[]}]}}";
    snprintf(out, out_size, "%s", json);
    return 0;
}

int labtwin_environment_dashboard_json(char *out, size_t out_size)
{
    const char *json =
        "{\"ok\":true,\"data\":{\"events\":[],"
        "\"last_event_seq\":7,\"storage_error\":false,"
        "\"sensor_stale\":false,\"last_telemetry_seq\":12,"
        "\"telemetry\":[{\"schema_version\":1,\"seq\":11,"
        "\"timestamp_epoch\":1770000000,\"temperature_avg\":23.4,"
        "\"humidity_avg\":47.5,\"valid_samples\":60,"
        "\"stale_samples\":0},{\"schema_version\":1,\"seq\":12,"
        "\"timestamp_epoch\":1770000030,\"temperature_avg\":23.5,"
        "\"humidity_avg\":48,\"valid_samples\":60,"
        "\"stale_samples\":0}]}}";
    snprintf(out, out_size, "%s", json);
    return 0;
}

int labtwin_sensor_get(labtwin_sensor_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->temperature_available = true;
    snapshot->humidity_available = true;
    snapshot->temperature_c = 23.5f;
    snapshot->humidity_percent = 48.0f;
    snapshot->sampled_epoch = 1770000001;
    return 0;
}

static char *request(labtwin_dashboard_session_t *session, const char *id,
                     const char *op, const char *payload)
{
    char message[512];
    char *response = NULL;
    bool handled;

    snprintf(message, sizeof(message),
        "{\"protocol\":\"labtwin.dashboard.v1\","
        "\"message_id\":\"%s\",\"kind\":\"request\","
        "\"op\":\"%s\",\"schema_version\":1,\"payload\":%s}",
        id, op, payload);
    handled = labtwin_dashboard_handle(session, message, strlen(message),
                                       &response);
    assert(handled);
    assert(response);
    return response;
}

static const char *kind_of(const char *text)
{
    static char kind[16];
    cJSON *root = cJSON_Parse(text);
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind"));
    snprintf(kind, sizeof(kind), "%s", value ? value : "");
    cJSON_Delete(root);
    return kind;
}

int main(void)
{
    labtwin_dashboard_session_t session;
    FILE *token;
    char *response;
    cJSON *root;
    cJSON *payload;
    cJSON *cursor;

    unlink(TOKEN_PATH);
    labtwin_dashboard_session_init(&session, true);

    response = request(&session, "1", "sync", "{}");
    assert(strcmp(kind_of(response), "error") == 0);
    free(response);

    response = request(&session, "2", "hello", "{\"token\":\"bad\"}");
    assert(strstr(response, "TOKEN_NOT_PROVISIONED"));
    free(response);

    token = fopen(TOKEN_PATH, "w");
    assert(token);
    assert(fputs(TEST_TOKEN "\n", token) >= 0);
    assert(fclose(token) == 0);

    response = request(&session, "3", "hello", "{\"token\":\"bad\"}");
    assert(strstr(response, "UNAUTHORIZED"));
    free(response);
    assert(!session.authenticated);

    response = request(&session, "4", "hello",
                       "{\"token\":\"" TEST_TOKEN "\"}");
    assert(strcmp(kind_of(response), "response") == 0);
    free(response);
    assert(session.authenticated);

    response = request(&session, "5", "sync", "{}");
    root = cJSON_Parse(response);
    assert(root);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "op")),
                  "sync") == 0);
    payload = cJSON_GetObjectItem(root, "payload");
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload, "experiments")) == 1);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(payload, "telemetry")) == 2);
    cursor = cJSON_GetObjectItem(payload, "cursor");
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cursor, "environment")) == 7);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(
        cJSON_GetObjectItem(cursor, "experiments"), "exp-1")) == 3);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cursor, "telemetry")) == 12);
    cJSON_Delete(root);
    free(response);

    response = request(&session, "6", "ping", "{}");
    root = cJSON_Parse(response);
    assert(root);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "kind")),
                  "response") == 0);
    assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(root, "op")),
                  "ping") == 0);
    payload = cJSON_GetObjectItem(root, "payload");
    assert(cJSON_IsObject(payload));
    cJSON_Delete(root);
    free(response);

    unlink(TOKEN_PATH);
    puts("labtwin dashboard host tests: PASS");
    return 0;
}
