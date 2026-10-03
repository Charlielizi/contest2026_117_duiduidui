/* Lightweight, confirmed memory for the on-device laboratory assistant. */
#pragma once

#include <stddef.h>

#define STRUCTURED_MEMORY_CONTEXT_MAX_BYTES (2 * 1024)

int structured_memory_init(void);
int structured_memory_propose_json(const char *input, char *output,
                                   size_t output_size);
int structured_memory_confirm(const char *channel, const char *chat_id,
                              char *output, size_t output_size);
int structured_memory_discard(const char *channel, const char *chat_id,
                              char *output, size_t output_size);
int structured_memory_forget(const char *channel, const char *chat_id,
                             const char *scope, const char *item_id,
                             char *output, size_t output_size);
int structured_memory_handle_command(const char *channel, const char *chat_id,
                                     const char *command, char *output,
                                     size_t output_size);
int structured_memory_build_context(const char *channel, const char *chat_id,
                                    const char *query, char *output,
                                    size_t output_size);
int structured_memory_is_confirmation_text(const char *text);
