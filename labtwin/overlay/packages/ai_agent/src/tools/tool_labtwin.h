#pragma once

#include <stddef.h>

int tool_experiment_create_execute(const char *input, char *output, size_t size);
int tool_experiment_update_execute(const char *input, char *output, size_t size);
int tool_experiment_delete_execute(const char *input, char *output, size_t size);
int tool_experiment_get_execute(const char *input, char *output, size_t size);
int tool_experiment_list_execute(const char *input, char *output, size_t size);
int tool_experiment_transition_execute(const char *input, char *output, size_t size);
int tool_timer_start_execute(const char *input, char *output, size_t size);
int tool_timer_cancel_execute(const char *input, char *output, size_t size);
int tool_lab_log_add_execute(const char *input, char *output, size_t size);
int tool_sensor_snapshot_execute(const char *input, char *output, size_t size);
int tool_environment_event_list_execute(const char *input, char *output,
                                        size_t size);
int tool_environment_event_ack_execute(const char *input, char *output,
                                       size_t size);

/* Protocol tools */
int tool_protocol_validate_execute(const char *input, char *output, size_t size);
int tool_protocol_create_execute(const char *input, char *output, size_t size);
int tool_protocol_get_execute(const char *input, char *output, size_t size);
int tool_protocol_list_execute(const char *input, char *output, size_t size);

/* Controller tools */
int tool_controller_create_execute(const char *input, char *output, size_t size);
int tool_controller_get_execute(const char *input, char *output, size_t size);
int tool_controller_command_execute(const char *input, char *output, size_t size);

/* Immutable report cards and read-only report search. */
int tool_report_create_execute(const char *input, char *output, size_t size);
int tool_report_search_execute(const char *input, char *output, size_t size);
int tool_report_get_execute(const char *input, char *output, size_t size);
