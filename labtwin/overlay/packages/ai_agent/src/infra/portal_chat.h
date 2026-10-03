#pragma once
#include <stdbool.h>
#include "cJSON.h"
/* Transport supplies authenticated owner; JSON never supplies this identity. */
int portal_chat_submit(const char *owner, const char *session_token, const char *id, const char *text, cJSON **result);
cJSON *portal_chat_get(const char *owner, const char *id);
int portal_chat_begin(const char *id);
bool portal_chat_can_execute(const char *id);
void portal_chat_finish(const char *id, const char *reply, const char *error);
int portal_chat_clear(void);
