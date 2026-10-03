#pragma once

#include <stdbool.h>
#include <stdint.h>

int admin_api_init(void);
bool admin_api_origin_valid(const char *request);
int admin_delete_experiment(const char *id, uint64_t sequence, const char *source);
bool admin_api_try_handle(int fd, const char *request, int request_len);
