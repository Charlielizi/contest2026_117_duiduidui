#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "cJSON.h"
/* owner is the authenticated session from the transport, never model input. */
int portal_tool_execute(const char *name, const char *input, const char *owner,
                        char *output, size_t output_size);
cJSON *portal_operations_get(const char *owner, const char *id);
int portal_operation_resolve(const char *owner, const char *id, cJSON *body,
                             bool cancel, cJSON **result);
