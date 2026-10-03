#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LABTWIN_CONTROLLER_SCHEMA_VERSION 1
#define LABTWIN_CONTROLLER_MAX_JSON_SIZE (16 * 1024)

/* Initialize the controller store under /data/labtwin/controllers. */
int labtwin_controller_init(void);

/* Create a run bound to one immutable protocol version. */
int labtwin_controller_create_json(const char *input, char *output,
                                   size_t output_size, const char *source);

/* Read the latest recoverable state for a run_id. */
int labtwin_controller_get_json(const char *input, char *output,
                                size_t output_size);

/* Apply one guarded state-machine action using expected_seq. */
int labtwin_controller_command_json(const char *input, char *output,
                                    size_t output_size, const char *source);

#ifdef __cplusplus
}
#endif
