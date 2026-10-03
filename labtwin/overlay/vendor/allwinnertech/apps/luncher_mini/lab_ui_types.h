#ifndef LAB_UI_TYPES_H
#define LAB_UI_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define LAB_UI_TIMER_COUNT 3
#define LAB_UI_EXPERIMENT_COUNT 8
#define LAB_UI_RECENT_COUNT 3

typedef enum
{
  LAB_UI_PAGE_IDLE = 0,
  LAB_UI_PAGE_RUNNING,
  LAB_UI_PAGE_TIMERS,
  LAB_UI_PAGE_VOICE,
  LAB_UI_PAGE_ALERT
} lab_ui_page_t;

typedef enum
{
  LAB_UI_CMD_PRIMARY = 0,
  LAB_UI_CMD_SECONDARY,
  LAB_UI_CMD_TERTIARY,
  LAB_UI_CMD_SETTINGS,
  LAB_UI_CMD_VOICE,
  LAB_UI_CMD_PARALLEL,
  LAB_UI_CMD_PICKER_CLOSE,
  LAB_UI_CMD_PICKER_NEW,
  LAB_UI_CMD_EXPERIMENT_SELECT
} lab_ui_command_t;

typedef struct
{
  lab_ui_command_t command;
  char experiment_id[32];
} lab_ui_action_t;

typedef struct
{
  char experiment[32];
  char task[48];
  int32_t remaining_seconds;
  bool expired;
} lab_ui_timer_item_t;

typedef struct
{
  char experiment_id[32];
  char name[64];
  char meta[48];
  bool ready;
  bool paused;
} lab_ui_experiment_item_t;

typedef struct
{
  lab_ui_page_t page;
  bool wifi_connected;
  bool microphone_ready;
  bool voice_confirmation_pending;
  bool paused;
  bool experiment_picker_open;
  bool weather_location_fallback;
  char clock[8];
  char date[48];
  char weather[32];
  char weather_temperature[16];
  uint8_t weather_code;
  char experiment[48];
  char step[72];
  char environment[80];
  char temperature[16];
  char humidity[16];
  char distance[16];
  int32_t remaining_seconds;
  uint8_t step_index;
  uint8_t step_count;
  lab_ui_timer_item_t timers[LAB_UI_TIMER_COUNT];
  uint8_t experiment_count;
  lab_ui_experiment_item_t experiments[LAB_UI_EXPERIMENT_COUNT];
  uint8_t recent_count;
  lab_ui_experiment_item_t recent[LAB_UI_RECENT_COUNT];
  char transcript[1600];
  char voice_status[40];
  char voice_meta[32];
  char alert_title[48];
  char alert_id[64];
  char alert_detail[96];
  char alert_current[16];
  char alert_threshold[16];
  bool alert_confirm_only;
  bool alert_no_actions;
} lab_ui_snapshot_t;

#endif /* LAB_UI_TYPES_H */
