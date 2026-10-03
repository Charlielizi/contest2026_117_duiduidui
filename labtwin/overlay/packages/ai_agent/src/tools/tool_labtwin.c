#include "tools/tool_labtwin.h"
#include "labtwin/labtwin.h"
#include "labtwin/labtwin_report.h"
#include <stdio.h>
#include <errno.h>

int tool_experiment_create_execute(const char *in, char *out, size_t size)
{ return labtwin_experiment_create_json(in, out, size, "agent"); }
int tool_experiment_update_execute(const char *in, char *out, size_t size)
{ return labtwin_experiment_update_json(in, out, size, "agent"); }
int tool_experiment_delete_execute(const char *in, char *out, size_t size)
{
    (void)in;
    snprintf(out, size, "{\"ok\":false,\"code\":\"EXPLICIT_ADMIN_CONFIRMATION_REQUIRED\"}");
    return -EACCES;
}
int tool_experiment_get_execute(const char *in, char *out, size_t size)
{ return labtwin_experiment_get_json(in, out, size); }
int tool_experiment_list_execute(const char *in, char *out, size_t size)
{ return labtwin_experiment_list_json(in, out, size); }
int tool_experiment_transition_execute(const char *in, char *out, size_t size)
{ return labtwin_experiment_transition_json(in, out, size, "agent"); }
int tool_timer_start_execute(const char *in, char *out, size_t size)
{ return labtwin_timer_start_json(in, out, size, "agent"); }
int tool_timer_cancel_execute(const char *in, char *out, size_t size)
{ return labtwin_timer_cancel_json(in, out, size, "agent"); }
int tool_lab_log_add_execute(const char *in, char *out, size_t size)
{ return labtwin_log_add_json(in, out, size, "agent"); }
int tool_sensor_snapshot_execute(const char *in, char *out, size_t size)
{ (void)in; return labtwin_sensor_snapshot_json(out, size); }
int tool_environment_event_list_execute(const char *in, char *out, size_t size)
{ return labtwin_environment_events_json(in, out, size); }
int tool_environment_event_ack_execute(const char *in, char *out, size_t size)
{ return labtwin_environment_ack_json(in, out, size, "agent"); }

#include "labtwin/labtwin_protocol.h"
#include "labtwin/labtwin_controller.h"

int tool_protocol_validate_execute(const char *in, char *out, size_t size)
{ return labtwin_protocol_validate_json(in, out, size); }
int tool_protocol_create_execute(const char *in, char *out, size_t size)
{ return labtwin_protocol_create_json(in, out, size, "agent"); }
int tool_protocol_get_execute(const char *in, char *out, size_t size)
{ return labtwin_protocol_get_json(in, out, size); }
int tool_protocol_list_execute(const char *in, char *out, size_t size)
{ return labtwin_protocol_list_json(in, out, size); }

int tool_controller_create_execute(const char *in, char *out, size_t size)
{ return labtwin_controller_create_json(in, out, size, "agent"); }
int tool_controller_get_execute(const char *in, char *out, size_t size)
{ return labtwin_controller_get_json(in, out, size); }
int tool_controller_command_execute(const char *in, char *out, size_t size)
{ return labtwin_controller_command_json(in, out, size, "agent"); }

int tool_report_create_execute(const char *in, char *out, size_t size)
{ return labtwin_report_create_json(in, out, size, "agent"); }
int tool_report_search_execute(const char *in, char *out, size_t size)
{ return labtwin_report_search_json(in, out, size); }
int tool_report_get_execute(const char *in, char *out, size_t size)
{ return labtwin_report_get_json(in, out, size); }
