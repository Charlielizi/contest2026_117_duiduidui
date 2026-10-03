/*
 * Copyright (C) 2026 Xiaomi Corporation
 * Licensed under the Apache License, Version 2.0
 */

#include "tools/tool_board_output.h"
#include "ui/agent_feedback_display.h"
#include "voice/voice_channel.h"
#include "agent_compat.h"

#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOARD_TEXT_MAX_BYTES 180
#define BOARD_SPEECH_MAX_BYTES 160
#define BOARD_TTS_STACKSIZE (24 * 1024)

typedef struct {
    char text[BOARD_SPEECH_MAX_BYTES + 1];
} board_speech_request_t;

static pthread_mutex_t s_tts_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_tts_active;

static int parse_text(const char *input_json, char *text, size_t text_size,
                      unsigned int *duration_seconds)
{
    cJSON *root;
    cJSON *item;
    size_t len;

    if (!input_json || !text || text_size < 2) {
        return -EINVAL;
    }
    root = cJSON_Parse(input_json);
    if (!root) {
        return -EINVAL;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_Delete(root);
        return -EINVAL;
    }
    len = strlen(item->valuestring);
    if (len == 0 || len >= text_size) {
        cJSON_Delete(root);
        return -E2BIG;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)item->valuestring[i];
        if (ch < 0x20 && ch != '\n') {
            cJSON_Delete(root);
            return -EINVAL;
        }
    }
    memcpy(text, item->valuestring, len + 1);

    if (duration_seconds) {
        *duration_seconds = 10;
        item = cJSON_GetObjectItemCaseSensitive(root, "duration_seconds");
        if (item) {
            if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
                item->valuedouble > 30 ||
                (double)(unsigned int)item->valuedouble != item->valuedouble) {
                cJSON_Delete(root);
                return -EINVAL;
            }
            *duration_seconds = (unsigned int)item->valuedouble;
        }
    }
    cJSON_Delete(root);
    return OK;
}

int tool_board_display_execute(const char *input_json, char *output,
                               size_t output_size)
{
    char text[BOARD_TEXT_MAX_BYTES + 1];
    unsigned int duration_seconds;
    int ret = parse_text(input_json, text, sizeof(text), &duration_seconds);
    if (ret != OK) {
        snprintf(output, output_size,
                 "{\"error\":\"text must be 1-180 bytes; duration_seconds must be 1-30\"}");
        return ERROR;
    }
    ret = agent_feedback_display_show(text, duration_seconds);
    if (ret != OK) {
        snprintf(output, output_size,
                 "{\"error\":\"board display unavailable\",\"code\":%d}", ret);
        return ERROR;
    }
    snprintf(output, output_size,
             "{\"ok\":true,\"shown\":true,\"duration_seconds\":%u}",
             duration_seconds);
    return OK;
}

static void *board_tts_worker(void *arg)
{
    board_speech_request_t *request = arg;
    int ret = voice_channel_speak(request->text);
    syslog(ret == OK ? LOG_INFO : LOG_ERR,
           "[board_tts] speech %s (rc=%d)\n",
           ret == OK ? "completed" : "failed", ret);
    free(request);
    pthread_mutex_lock(&s_tts_lock);
    s_tts_active = false;
    pthread_mutex_unlock(&s_tts_lock);
    return NULL;
}

int tool_board_speak_execute(const char *input_json, char *output,
                             size_t output_size)
{
    board_speech_request_t *request;
    pthread_attr_t attr;
    pthread_t thread;
    int ret;

    request = calloc(1, sizeof(*request));
    if (!request) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }
    ret = parse_text(input_json, request->text, sizeof(request->text), NULL);
    if (ret != OK) {
        free(request);
        snprintf(output, output_size, "{\"error\":\"text must be 1-160 bytes\"}");
        return ERROR;
    }

    pthread_mutex_lock(&s_tts_lock);
    if (s_tts_active) {
        pthread_mutex_unlock(&s_tts_lock);
        free(request);
        snprintf(output, output_size,
                 "{\"error\":\"board speech already in progress\"}");
        return ERROR;
    }
    s_tts_active = true;
    pthread_mutex_unlock(&s_tts_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BOARD_TTS_STACKSIZE);
    ret = pthread_create(&thread, &attr, board_tts_worker, request);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        pthread_mutex_lock(&s_tts_lock);
        s_tts_active = false;
        pthread_mutex_unlock(&s_tts_lock);
        free(request);
        snprintf(output, output_size,
                 "{\"error\":\"board speech worker unavailable\",\"code\":%d}", ret);
        return ERROR;
    }
    pthread_detach(thread);
    snprintf(output, output_size, "{\"ok\":true,\"speech\":\"queued\"}");
    return OK;
}
