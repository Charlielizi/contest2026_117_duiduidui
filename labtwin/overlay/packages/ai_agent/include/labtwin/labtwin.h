#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LABTWIN_MAX_EXPERIMENTS 16
#define LABTWIN_MAX_ACTIVE_EXPERIMENTS 8
#define LABTWIN_MAX_STEPS 16
#define LABTWIN_MAX_TIMERS 16
#define LABTWIN_UI_TIMERS 3
#define LABTWIN_ENV_RULE_COUNT 4
#define LABTWIN_ENV_EVENT_ID_MAX 24

typedef enum {
    LABTWIN_STATE_READY = 0,
    LABTWIN_STATE_RUNNING,
    LABTWIN_STATE_PAUSED,
    LABTWIN_STATE_COMPLETED,
    LABTWIN_STATE_CANCELLED,
    LABTWIN_STATE_RECOVERY_ERROR
} labtwin_state_t;

typedef enum {
    LABTWIN_TIMER_RUNNING = 0,
    LABTWIN_TIMER_PAUSED,
    LABTWIN_TIMER_EXPIRED,
    LABTWIN_TIMER_CANCELLED
} labtwin_timer_state_t;

typedef struct {
    char id[16];
    char label[64];
    uint32_t remaining_seconds;
    uint8_t step_index;
    labtwin_timer_state_t state;
} labtwin_timer_view_t;

typedef struct {
    char experiment_id[32];
    char name[64];
    char step[96];
    char recovery_reason[32];
    labtwin_state_t state;
    uint8_t step_index;
    uint8_t step_count;
    uint8_t timer_count;
    labtwin_timer_view_t timers[LABTWIN_UI_TIMERS];
} labtwin_experiment_view_t;

typedef struct {
    char experiment_id[32];
    char name[64];
    labtwin_state_t state;
    uint8_t current_step;
    uint8_t step_count;
    bool has_timer;
    bool timer_expired;
    int32_t primary_remaining_seconds;
    int64_t updated_epoch;
} labtwin_experiment_summary_t;

typedef struct {
    bool temperature_available;
    bool humidity_available;
    bool proximity_available;
    float temperature_c;
    float humidity_percent;
    float proximity_cm;
    int64_t sampled_epoch;
    uint64_t temperature_sampled_ms;
    uint64_t humidity_sampled_ms;
    uint32_t temperature_age_ms;
    uint32_t humidity_age_ms;
    uint32_t temperature_failures;
    uint32_t humidity_failures;
} labtwin_sensor_snapshot_t;

typedef enum {
    LABTWIN_ENV_EVENT_ACTIVE = 0,
    LABTWIN_ENV_EVENT_ACKNOWLEDGED,
    LABTWIN_ENV_EVENT_RECOVERED
} labtwin_environment_event_state_t;

typedef struct {
    char event_id[LABTWIN_ENV_EVENT_ID_MAX];
    char rule_id[20];
    char sensor[16];
    char experiment_id[32];
    char ack_source[16];
    labtwin_environment_event_state_t state;
    float measured_value;
    float trigger_threshold;
    float clear_threshold;
    int64_t created_epoch;
    int64_t acknowledged_epoch;
    int64_t recovered_epoch;
} labtwin_environment_event_view_t;

int labtwin_service_init(void);
bool labtwin_clock_trusted(void);

int labtwin_experiment_create_json(const char *input, char *output,
                                   size_t output_size, const char *source);
int labtwin_experiment_get_json(const char *input, char *output,
                                size_t output_size);
int labtwin_experiment_list_json(const char *input, char *output,
                                 size_t output_size);
/* Disk-backed calendar/history query.  Unlike the live snapshot this includes
 * terminal experiments that have been evicted from the small runtime cache. */
int labtwin_experiment_history_json(const char *input, char *output,
                                    size_t output_size);
int labtwin_experiment_update_json(const char *input, char *output,
                                   size_t output_size, const char *source);
void labtwin_experiment_forget(const char *experiment_id);
int labtwin_experiment_snapshot_json(char *output, size_t output_size);
int labtwin_experiment_transition_json(const char *input, char *output,
                                       size_t output_size,
                                       const char *source);
int labtwin_timer_start_json(const char *input, char *output,
                             size_t output_size, const char *source);
int labtwin_timer_cancel_json(const char *input, char *output,
                              size_t output_size, const char *source);
int labtwin_log_add_json(const char *input, char *output,
                         size_t output_size, const char *source);
int labtwin_sensor_snapshot_json(char *output, size_t output_size);

int labtwin_get_focused_view(labtwin_experiment_view_t *view);
int labtwin_focus_next(void);
int labtwin_focus_experiment(const char *experiment_id);
int labtwin_experiment_list_summaries(labtwin_experiment_summary_t *items,
                                      size_t capacity, size_t *count);
int labtwin_ui_transition(const char *action);
void labtwin_sensor_update(bool temp_valid, float temperature_c,
                           bool humidity_valid, float humidity_percent,
                           bool proximity_valid, float proximity_cm);
void labtwin_sensor_failure(bool temperature_failed, bool humidity_failed);
int labtwin_sensor_get(labtwin_sensor_snapshot_t *snapshot);
int labtwin_environment_status_json(char *output, size_t output_size);
int labtwin_environment_events_json(const char *input, char *output,
                                    size_t output_size);
int labtwin_environment_dashboard_json(char *output, size_t output_size);
int labtwin_environment_get_json(const char *input, char *output,
                                 size_t output_size);
int labtwin_environment_ack_json(const char *input, char *output,
                                 size_t output_size, const char *source);
int labtwin_environment_rule_show_json(char *output, size_t output_size);
int labtwin_environment_rule_set_json(const char *input, char *output,
                                      size_t output_size);
int labtwin_environment_rules_set_json(const char *input, char *output,
                                       size_t output_size);
int labtwin_environment_rule_reset_json(char *output, size_t output_size);
int labtwin_environment_get_active(labtwin_environment_event_view_t *event);
int labtwin_environment_get_recovery_notice(
    labtwin_environment_event_view_t *event);
bool labtwin_environment_has_storage_error(void);
void labtwin_environment_set_foreground(const char *experiment_id);
int labtwin_environment_resolve_experiment(const char *preferred,
                                           char *output,
                                           size_t output_size);
#ifdef CONFIG_AI_AGENT_LABTWIN_ENV_TEST
int labtwin_environment_inject(const char *sensor, float value, bool clear);
#endif
const char *labtwin_state_name(labtwin_state_t state);
/* callback runs under the experiment lock and must not re-enter LabTwin. */
int labtwin_experiment_delete_locked(const char *id, uint64_t expected_seq,
                                    int (*remove_files)(const char *, void *), void *context);

#ifdef __cplusplus
}
#endif
