#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LABTWIN_PROTOCOL_SCHEMA_VERSION 1
#define LABTWIN_PROTOCOL_MAX_PARAMETERS 16
#define LABTWIN_PROTOCOL_MAX_PREREQUISITES 8
#define LABTWIN_PROTOCOL_MAX_STEPS 16
#define LABTWIN_PROTOCOL_MAX_ACCEPTANCE_CRITERIA 16
#define LABTWIN_PROTOCOL_MAX_STORED_VERSIONS 64
#define LABTWIN_PROTOCOL_MAX_JSON_SIZE (16 * 1024)

/* Initialize the immutable protocol store under /data/labtwin/protocols. */
int labtwin_protocol_init(void);

/* Validate a complete Protocol v1 document without writing it. */
int labtwin_protocol_validate_json(const char *input, char *output,
                                   size_t output_size);

/* Persist a complete document. Versions must begin at 1 and increase by one. */
int labtwin_protocol_create_json(const char *input, char *output,
                                 size_t output_size, const char *source);

/* Read one immutable protocol version by protocol_id and version. */
int labtwin_protocol_get_json(const char *input, char *output,
                              size_t output_size);

/* List stored protocol summaries, optionally filtered by protocol_id. */
int labtwin_protocol_list_json(const char *input, char *output,
                               size_t output_size);

#ifdef __cplusplus
}
#endif
