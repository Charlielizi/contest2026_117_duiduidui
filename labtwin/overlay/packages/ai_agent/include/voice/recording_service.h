/* Persistent administrator-controlled microphone recording service. */
#pragma once

#include <stddef.h>

struct cJSON;

int recording_service_init(void);
int recording_service_start(char *id, size_t id_size);
int recording_service_stop(const char *id);
int recording_service_delete(const char *id);
int recording_service_resolve_audio(const char *id, char *path,
                                    size_t path_size,
                                    unsigned long long *file_size);
struct cJSON *recording_service_snapshot_json(void);
