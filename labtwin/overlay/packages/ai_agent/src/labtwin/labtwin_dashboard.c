/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "labtwin/labtwin.h"
#include "labtwin/labtwin_dashboard.h"

#ifndef LABTWIN_DASHBOARD_HOST_TEST
#include "agent_config.h"
#else
#define AGENT_NODE_ID "labtwin-host-test"
#define AGENT_BUILD_VERSION "host-test"
#endif

#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LABTWIN_DASHBOARD_TOKEN_PATH
#ifdef LABTWIN_DASHBOARD_HOST_TEST
#define LABTWIN_DASHBOARD_TOKEN_PATH "/tmp/labtwin-dashboard-token"
#else
#define LABTWIN_DASHBOARD_TOKEN_PATH "/data/labtwin/dashboard.token"
#endif
#endif

#define DASHBOARD_SCHEMA_VERSION 1
#define DASHBOARD_EXPERIMENT_BUFFER (48 * 1024)
#define DASHBOARD_ENVIRONMENT_BUFFER (48 * 1024)
#define DASHBOARD_MAX_FRAME 65535
#define DASHBOARD_TOKEN_MAX 128

static char g_boot_id[64];
static pthread_once_t g_boot_id_once = PTHREAD_ONCE_INIT;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void initialize_boot_id(void)
{
    snprintf(g_boot_id, sizeof(g_boot_id), "%s-%llu", AGENT_NODE_ID,
             (unsigned long long)monotonic_ms());
}

static const char *boot_id(void)
{
    pthread_once(&g_boot_id_once, initialize_boot_id);
    return g_boot_id;
}

static char *print_envelope(const char *message_id, const char *kind,
                            const char *op, cJSON *payload)
{
    cJSON *root = cJSON_CreateObject();
    char *text;

    if (!root) {
        cJSON_Delete(payload);
        return NULL;
    }
    cJSON_AddStringToObject(root, "protocol", LABTWIN_DASHBOARD_PROTOCOL);
    cJSON_AddStringToObject(root, "message_id", message_id ? message_id : "");
    cJSON_AddStringToObject(root, "kind", kind);
    cJSON_AddStringToObject(root, "op", op ? op : "unknown");
    cJSON_AddNumberToObject(root, "schema_version", DASHBOARD_SCHEMA_VERSION);
    cJSON_AddItemToObject(root, "payload", payload ? payload : cJSON_CreateObject());
    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text && strlen(text) > DASHBOARD_MAX_FRAME) {
        free(text);
        return NULL;
    }
    return text;
}

static char *error_envelope(const char *message_id, const char *op,
                            const char *code, const char *message)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "code", code);
    cJSON_AddStringToObject(payload, "message", message);
    return print_envelope(message_id, "error", op, payload);
}

static int read_token(char token[DASHBOARD_TOKEN_MAX])
{
    FILE *file = fopen(LABTWIN_DASHBOARD_TOKEN_PATH, "r");
    size_t length;

    if (!file)
        return -errno;
    if (!fgets(token, DASHBOARD_TOKEN_MAX, file)) {
        int ret = ferror(file) ? -EIO : -EINVAL;
        fclose(file);
        return ret;
    }
    fclose(file);
    length = strcspn(token, "\r\n");
    token[length] = '\0';
    return length >= 16 ? 0 : -EINVAL;
}

static bool token_equal(const char *supplied, const char *expected)
{
    size_t supplied_len = supplied ? strlen(supplied) : 0;
    size_t expected_len = strlen(expected);
    size_t i;
    size_t difference = supplied_len ^ expected_len;

    for (i = 0; i < expected_len; i++) {
        unsigned char value = i < supplied_len ? (unsigned char)supplied[i] : 0;
        difference |= (size_t)(value ^ (unsigned char)expected[i]);
    }
    return difference == 0;
}

static char *handle_hello(labtwin_dashboard_session_t *session,
                          const cJSON *request, const char *message_id)
{
    const cJSON *payload = cJSON_GetObjectItem(request, "payload");
    const cJSON *token_item = payload ? cJSON_GetObjectItem(payload, "token") : NULL;
    char expected[DASHBOARD_TOKEN_MAX] = { 0 };
    int ret;
    cJSON *response;
    cJSON *capabilities;

    if (!session->authenticated) {
        ret = read_token(expected);
        if (ret != 0) {
            return error_envelope(message_id, "hello", "TOKEN_NOT_PROVISIONED",
                                  "dashboard token is not provisioned");
        }
        if (!cJSON_IsString(token_item) ||
            !token_equal(token_item->valuestring, expected)) {
            return error_envelope(message_id, "hello", "UNAUTHORIZED",
                                  "dashboard token is invalid");
        }
    }

    session->authenticated = true;
    response = cJSON_CreateObject();
    capabilities = cJSON_AddArrayToObject(response, "capabilities");
    cJSON_AddStringToObject(response, "device_id", AGENT_NODE_ID);
    cJSON_AddStringToObject(response, "boot_id", boot_id());
    cJSON_AddStringToObject(response, "firmware", AGENT_BUILD_VERSION);
    cJSON_AddStringToObject(response, "protocol", LABTWIN_DASHBOARD_PROTOCOL);
    cJSON_AddBoolToObject(response, "read_only", true);
    cJSON_AddBoolToObject(response, "clock_trusted", labtwin_clock_trusted());
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("snapshot"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("cursor"));
    return print_envelope(message_id, "response", "hello", response);
}

static cJSON *response_data(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    cJSON *data;

    if (!root)
        return NULL;
    data = cJSON_DetachItemFromObject(root, "data");
    cJSON_Delete(root);
    return data;
}

static char *handle_sync(const char *message_id)
{
    char *experiments_text = calloc(1, DASHBOARD_EXPERIMENT_BUFFER);
    char *environment_text = calloc(1, DASHBOARD_ENVIRONMENT_BUFFER);
    cJSON *experiment_data = NULL;
    cJSON *environment_data = NULL;
    cJSON *payload = NULL;
    cJSON *device;
    cJSON *cursor;
    cJSON *experiment_cursors;
    cJSON *experiments;
    cJSON *environment_events;
    cJSON *telemetry;
    cJSON *events;
    labtwin_sensor_snapshot_t sensor = { 0 };
    uint64_t environment_seq;
    uint64_t telemetry_seq;
    int i;
    char *result = NULL;

    if (!experiments_text || !environment_text)
        goto memory_error;
    if (labtwin_experiment_snapshot_json(experiments_text,
                                         DASHBOARD_EXPERIMENT_BUFFER) != 0 ||
        labtwin_environment_dashboard_json(environment_text,
                                            DASHBOARD_ENVIRONMENT_BUFFER) != 0)
        goto snapshot_error;
    experiment_data = response_data(experiments_text);
    environment_data = response_data(environment_text);
    if (!experiment_data || !environment_data)
        goto snapshot_error;

    experiments = cJSON_DetachItemFromObject(experiment_data, "experiments");
    environment_events = cJSON_DetachItemFromObject(environment_data, "events");
    telemetry = cJSON_DetachItemFromObject(environment_data, "telemetry");
    environment_seq = (uint64_t)cJSON_GetNumberValue(
        cJSON_GetObjectItem(environment_data, "last_event_seq"));
    telemetry_seq = (uint64_t)cJSON_GetNumberValue(
        cJSON_GetObjectItem(environment_data, "last_telemetry_seq"));
    labtwin_sensor_get(&sensor);

    payload = cJSON_CreateObject();
    device = cJSON_AddObjectToObject(payload, "device");
    cJSON_AddStringToObject(device, "device_id", AGENT_NODE_ID);
    cJSON_AddStringToObject(device, "boot_id", boot_id());
    cJSON_AddStringToObject(device, "firmware", AGENT_BUILD_VERSION);
    cJSON_AddStringToObject(device, "protocol", LABTWIN_DASHBOARD_PROTOCOL);
    cJSON_AddBoolToObject(device, "clock_trusted", labtwin_clock_trusted());
    cJSON_AddBoolToObject(device, "storage_error",
        cJSON_IsTrue(cJSON_GetObjectItem(environment_data, "storage_error")));
    if (sensor.sampled_epoch > 0)
        cJSON_AddNumberToObject(device, "sampled_epoch", (double)sensor.sampled_epoch);
    else
        cJSON_AddNullToObject(device, "sampled_epoch");
    cJSON_AddBoolToObject(device, "sensor_stale",
        cJSON_IsTrue(cJSON_GetObjectItem(environment_data, "sensor_stale")));
    cJSON_AddItemToObject(payload, "experiments",
                          experiments ? experiments : cJSON_CreateArray());
    cJSON_AddItemToObject(payload, "environment_events",
                          environment_events ? environment_events : cJSON_CreateArray());

    cJSON_AddItemToObject(payload, "telemetry",
                          telemetry ? telemetry : cJSON_CreateArray());
    events = cJSON_AddArrayToObject(payload, "events");
    (void)events;

    cursor = cJSON_AddObjectToObject(payload, "cursor");
    cJSON_AddStringToObject(cursor, "boot_id", boot_id());
    experiment_cursors = cJSON_AddObjectToObject(cursor, "experiments");
    experiments = cJSON_GetObjectItem(payload, "experiments");
    for (i = 0; i < cJSON_GetArraySize(experiments); i++) {
        cJSON *experiment = cJSON_GetArrayItem(experiments, i);
        const char *id = cJSON_GetStringValue(
            cJSON_GetObjectItem(experiment, "experiment_id"));
        if (id)
            cJSON_AddNumberToObject(experiment_cursors, id,
                cJSON_GetNumberValue(cJSON_GetObjectItem(experiment,
                                                         "last_event_seq")));
    }
    cJSON_AddNumberToObject(cursor, "environment", (double)environment_seq);
    cJSON_AddNumberToObject(cursor, "telemetry", (double)telemetry_seq);

    result = print_envelope(message_id, "response", "sync", payload);
    payload = NULL;
    if (!result)
        goto snapshot_error;
    goto done;

memory_error:
    result = error_envelope(message_id, "sync", "OUT_OF_MEMORY",
                            "not enough memory for dashboard snapshot");
    goto done;
snapshot_error:
    result = error_envelope(message_id, "sync", "SNAPSHOT_FAILED",
                            "dashboard snapshot could not be generated");
done:
    free(experiments_text);
    free(environment_text);
    cJSON_Delete(experiment_data);
    cJSON_Delete(environment_data);
    cJSON_Delete(payload);
    return result;
}

int labtwin_dashboard_snapshot_json(char *out, size_t out_size)
{
    char *envelope;
    char *printed = NULL;
    cJSON *root = NULL;
    cJSON *payload;
    int ret = -1;

    if (!out || out_size == 0)
        return -1;
    envelope = handle_sync("rest-snapshot");
    if (!envelope)
        return -1;
    root = cJSON_Parse(envelope);
    payload = root ? cJSON_GetObjectItem(root, "payload") : NULL;
    if (!payload)
        goto done;
    printed = cJSON_PrintUnformatted(payload);
    if (!printed || strlen(printed) >= out_size)
        goto done;
    snprintf(out, out_size, "%s", printed);
    ret = 0;
done:
    free(printed);
    cJSON_Delete(root);
    free(envelope);
    return ret;
}

void labtwin_dashboard_session_init(labtwin_dashboard_session_t *session,
                                    bool claimed)
{
    if (!session)
        return;
    session->claimed = claimed;
    session->authenticated = false;
}

bool labtwin_dashboard_handle(labtwin_dashboard_session_t *session,
                              const char *message, size_t message_size,
                              char **response)
{
    cJSON *request;
    const cJSON *protocol;
    const cJSON *kind;
    const cJSON *op;
    const cJSON *message_id;
    const cJSON *schema_version;

    if (!session || !message || !response)
        return false;
    *response = NULL;
    request = cJSON_ParseWithLength(message, message_size);
    if (!request) {
        if (!session->claimed)
            return false;
        *response = error_envelope("", "unknown", "INVALID_JSON",
                                   "request is not valid JSON");
        return true;
    }

    protocol = cJSON_GetObjectItem(request, "protocol");
    if (!session->claimed &&
        (!cJSON_IsString(protocol) ||
         strcmp(protocol->valuestring, LABTWIN_DASHBOARD_PROTOCOL) != 0)) {
        cJSON_Delete(request);
        return false;
    }
    session->claimed = true;
    kind = cJSON_GetObjectItem(request, "kind");
    op = cJSON_GetObjectItem(request, "op");
    message_id = cJSON_GetObjectItem(request, "message_id");
    schema_version = cJSON_GetObjectItem(request, "schema_version");
    if (!cJSON_IsString(protocol) ||
        strcmp(protocol->valuestring, LABTWIN_DASHBOARD_PROTOCOL) != 0 ||
        !cJSON_IsString(kind) || strcmp(kind->valuestring, "request") != 0 ||
        !cJSON_IsString(op) || !cJSON_IsString(message_id) ||
        !cJSON_IsNumber(schema_version) || schema_version->valueint != 1) {
        *response = error_envelope(cJSON_IsString(message_id) ? message_id->valuestring : "",
                                   cJSON_IsString(op) ? op->valuestring : "unknown",
                                   "INVALID_REQUEST", "invalid dashboard envelope");
    } else if (strcmp(op->valuestring, "hello") == 0) {
        *response = handle_hello(session, request, message_id->valuestring);
    } else if (!session->authenticated) {
        *response = error_envelope(message_id->valuestring, op->valuestring,
                                   "UNAUTHORIZED", "hello authentication required");
    } else if (strcmp(op->valuestring, "sync") == 0) {
        *response = handle_sync(message_id->valuestring);
    } else if (strcmp(op->valuestring, "ping") == 0) {
        /* The dashboard sends application-level heartbeats while the
         * WebSocket is otherwise idle.  Acknowledge them so the browser does
         * not turn a healthy connection into a persistent error state. */
        *response = print_envelope(message_id->valuestring, "response",
                                   "ping", cJSON_CreateObject());
    } else {
        *response = error_envelope(message_id->valuestring, op->valuestring,
                                   "UNSUPPORTED_OPERATION",
                                   "dashboard operation is not supported");
    }
    cJSON_Delete(request);
    return true;
}
