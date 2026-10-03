/* Immutable, compact report cards for LabTwin experiment evidence. */
#pragma once

#include <stddef.h>

int labtwin_report_init(void);
int labtwin_report_create_json(const char *input, char *output,
                               size_t output_size, const char *source);
int labtwin_report_search_json(const char *input, char *output,
                               size_t output_size);
int labtwin_report_get_json(const char *input, char *output,
                            size_t output_size);
