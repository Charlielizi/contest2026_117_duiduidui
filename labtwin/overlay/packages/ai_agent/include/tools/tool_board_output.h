/* Controlled board display and speech tools for Agent requests. */
#pragma once

#include <stddef.h>

int tool_board_display_execute(const char *input_json, char *output,
                               size_t output_size);
int tool_board_speak_execute(const char *input_json, char *output,
                             size_t output_size);
