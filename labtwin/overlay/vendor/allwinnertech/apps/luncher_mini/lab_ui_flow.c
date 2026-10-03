#include "lab_ui_flow.h"

#include <string.h>

static lab_ui_page_t content_page(bool have_view)
{
  return have_view ? LAB_UI_PAGE_RUNNING : LAB_UI_PAGE_IDLE;
}

void lab_ui_flow_init(lab_ui_flow_t *flow)
{
  if (!flow) return;
  memset(flow, 0, sizeof(*flow));
  flow->page = LAB_UI_PAGE_IDLE;
  flow->voice_return_page = LAB_UI_PAGE_IDLE;
  flow->alert_return_page = LAB_UI_PAGE_IDLE;
}

void lab_ui_flow_sync_content(lab_ui_flow_t *flow, bool have_view)
{
  if (!flow || flow->picker_open || flow->page == LAB_UI_PAGE_VOICE ||
      flow->page == LAB_UI_PAGE_ALERT || flow->page == LAB_UI_PAGE_TIMERS)
    return;
  flow->page = content_page(have_view);
}

void lab_ui_flow_show_alert(lab_ui_flow_t *flow, lab_ui_alert_kind_t kind)
{
  if (!flow) return;
  if (flow->page != LAB_UI_PAGE_ALERT)
    flow->alert_return_page = flow->page;
  flow->picker_open = false;
  flow->alert_kind = kind;
  flow->page = LAB_UI_PAGE_ALERT;
}

void lab_ui_flow_finish_alert(lab_ui_flow_t *flow, bool have_view)
{
  lab_ui_page_t target;
  lab_ui_alert_kind_t kind;
  if (!flow) return;
  kind = flow->alert_kind;
  target = flow->alert_return_page;
  if (target == LAB_UI_PAGE_ALERT ||
      (target == LAB_UI_PAGE_VOICE && kind != LAB_UI_ALERT_TIMER_EXPIRED) ||
      (target == LAB_UI_PAGE_RUNNING && !have_view))
    target = content_page(have_view);
  flow->page = target;
  flow->alert_kind = LAB_UI_ALERT_NONE;
}

void lab_ui_flow_finish_voice(lab_ui_flow_t *flow, bool have_view)
{
  lab_ui_page_t target;
  if (!flow) return;
  target = flow->voice_return_page;
  if (target == LAB_UI_PAGE_VOICE || target == LAB_UI_PAGE_ALERT ||
      (target == LAB_UI_PAGE_RUNNING && !have_view))
    target = content_page(have_view);
  flow->page = target;
  flow->picker_open = flow->voice_return_picker && target == LAB_UI_PAGE_IDLE;
  flow->voice_return_picker = false;
}

static lab_ui_effect_t start_voice(lab_ui_flow_t *flow, bool new_experiment)
{
  flow->voice_return_page = flow->page;
  flow->voice_return_picker = flow->picker_open;
  flow->picker_open = false;
  flow->page = LAB_UI_PAGE_VOICE;
  return new_experiment ? LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE :
                          LAB_UI_EFFECT_START_VOICE;
}

lab_ui_effect_t lab_ui_flow_handle(lab_ui_flow_t *flow,
                                   const lab_ui_action_t *action,
                                   const lab_ui_flow_context_t *context)
{
  if (!flow || !action || !context) return LAB_UI_EFFECT_NONE;
  if (action->command == LAB_UI_CMD_SETTINGS)
    return LAB_UI_EFFECT_OPEN_SETTINGS;
  if (action->command == LAB_UI_CMD_VOICE)
    return start_voice(flow, false);

  if (flow->picker_open)
    {
      if (action->command == LAB_UI_CMD_PICKER_CLOSE)
        flow->picker_open = false;
      else if (action->command == LAB_UI_CMD_PICKER_NEW)
        return start_voice(flow, true);
      else if (action->command == LAB_UI_CMD_EXPERIMENT_SELECT)
        return LAB_UI_EFFECT_FOCUS_EXPERIMENT;
      return LAB_UI_EFFECT_NONE;
    }

  switch (flow->page)
    {
      case LAB_UI_PAGE_IDLE:
        if (action->command == LAB_UI_CMD_PRIMARY)
          flow->picker_open = true;
        else if (action->command == LAB_UI_CMD_SECONDARY)
          return start_voice(flow, false);
        else if (action->command == LAB_UI_CMD_TERTIARY ||
                 action->command == LAB_UI_CMD_PARALLEL)
          flow->page = LAB_UI_PAGE_TIMERS;
        break;
      case LAB_UI_PAGE_RUNNING:
        if (action->command == LAB_UI_CMD_PRIMARY)
          return LAB_UI_EFFECT_TOGGLE_PAUSE;
        if (action->command == LAB_UI_CMD_SECONDARY)
          {
            if (context->last_step)
              lab_ui_flow_show_alert(flow, LAB_UI_ALERT_COMPLETE);
            else
              return LAB_UI_EFFECT_COMPLETE_STEP;
          }
        else if (action->command == LAB_UI_CMD_TERTIARY)
          lab_ui_flow_show_alert(flow, LAB_UI_ALERT_CANCEL);
        else if (action->command == LAB_UI_CMD_PARALLEL)
          flow->page = LAB_UI_PAGE_TIMERS;
        break;
      case LAB_UI_PAGE_TIMERS:
        if (action->command == LAB_UI_CMD_PRIMARY)
          flow->page = content_page(context->have_view);
        else if (action->command == LAB_UI_CMD_SECONDARY)
          return LAB_UI_EFFECT_FOCUS_NEXT;
        else if (action->command == LAB_UI_CMD_TERTIARY && context->have_view)
          lab_ui_flow_show_alert(flow, LAB_UI_ALERT_CANCEL);
        break;
      case LAB_UI_PAGE_VOICE:
        if (action->command == LAB_UI_CMD_PRIMARY)
          return LAB_UI_EFFECT_CANCEL_VOICE;
        if (action->command == LAB_UI_CMD_SECONDARY &&
            context->voice_confirmation_pending)
          return LAB_UI_EFFECT_CONFIRM_VOICE;
        break;
      case LAB_UI_PAGE_ALERT:
        if (action->command == LAB_UI_CMD_SECONDARY)
          {
            lab_ui_flow_finish_alert(flow, context->have_view);
            return LAB_UI_EFFECT_NONE;
          }
        if (action->command != LAB_UI_CMD_PRIMARY) break;
        if (flow->alert_kind == LAB_UI_ALERT_ENVIRONMENT)
          return LAB_UI_EFFECT_ACK_ENVIRONMENT;
        if (flow->alert_kind == LAB_UI_ALERT_TIME_RECOVERY)
          return LAB_UI_EFFECT_RESUME_RECOVERY;
        if (flow->alert_kind == LAB_UI_ALERT_TIMER_EXPIRED)
          {
            lab_ui_flow_finish_alert(flow, context->have_view);
            return LAB_UI_EFFECT_NONE;
          }
        if (flow->alert_kind == LAB_UI_ALERT_COMPLETE)
          return LAB_UI_EFFECT_COMPLETE_EXPERIMENT;
        if (flow->alert_kind == LAB_UI_ALERT_CANCEL)
          return LAB_UI_EFFECT_CANCEL_EXPERIMENT;
        break;
    }
  return LAB_UI_EFFECT_NONE;
}
