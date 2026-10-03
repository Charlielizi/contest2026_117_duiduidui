#ifndef LAB_UI_FLOW_H
#define LAB_UI_FLOW_H

#include "lab_ui_types.h"

typedef enum
{
  LAB_UI_ALERT_NONE = 0,
  LAB_UI_ALERT_ENVIRONMENT,
  LAB_UI_ALERT_RECOVERY_NOTICE,
  LAB_UI_ALERT_TIME_RECOVERY,
  LAB_UI_ALERT_TIMER_EXPIRED,
  LAB_UI_ALERT_COMPLETE,
  LAB_UI_ALERT_CANCEL
} lab_ui_alert_kind_t;

typedef enum
{
  LAB_UI_EFFECT_NONE = 0,
  LAB_UI_EFFECT_OPEN_SETTINGS,
  LAB_UI_EFFECT_START_VOICE,
  LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE,
  LAB_UI_EFFECT_FOCUS_EXPERIMENT,
  LAB_UI_EFFECT_TOGGLE_PAUSE,
  LAB_UI_EFFECT_COMPLETE_STEP,
  LAB_UI_EFFECT_FOCUS_NEXT,
  LAB_UI_EFFECT_ACK_ENVIRONMENT,
  LAB_UI_EFFECT_RESUME_RECOVERY,
  LAB_UI_EFFECT_COMPLETE_EXPERIMENT,
  LAB_UI_EFFECT_CANCEL_EXPERIMENT,
  LAB_UI_EFFECT_CANCEL_VOICE,
  LAB_UI_EFFECT_CONFIRM_VOICE
} lab_ui_effect_t;

typedef struct
{
  bool have_view;
  bool paused;
  bool last_step;
  bool voice_confirmation_pending;
} lab_ui_flow_context_t;

typedef struct
{
  lab_ui_page_t page;
  lab_ui_page_t voice_return_page;
  lab_ui_page_t alert_return_page;
  lab_ui_alert_kind_t alert_kind;
  bool picker_open;
  bool voice_return_picker;
} lab_ui_flow_t;

void lab_ui_flow_init(lab_ui_flow_t *flow);
void lab_ui_flow_sync_content(lab_ui_flow_t *flow, bool have_view);
void lab_ui_flow_show_alert(lab_ui_flow_t *flow, lab_ui_alert_kind_t kind);
void lab_ui_flow_finish_alert(lab_ui_flow_t *flow, bool have_view);
void lab_ui_flow_finish_voice(lab_ui_flow_t *flow, bool have_view);
lab_ui_effect_t lab_ui_flow_handle(lab_ui_flow_t *flow,
                                   const lab_ui_action_t *action,
                                   const lab_ui_flow_context_t *context);

#endif
