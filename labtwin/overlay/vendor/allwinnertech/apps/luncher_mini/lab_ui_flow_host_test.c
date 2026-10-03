#include "lab_ui_flow.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static lab_ui_action_t action(lab_ui_command_t command)
{
  lab_ui_action_t value;
  memset(&value, 0, sizeof(value));
  value.command = command;
  return value;
}

int main(void)
{
  lab_ui_flow_t flow;
  lab_ui_flow_context_t ctx = {0};
  lab_ui_action_t cmd;
  lab_ui_flow_init(&flow);
  assert(flow.page == LAB_UI_PAGE_IDLE);
  cmd = action(LAB_UI_CMD_SETTINGS);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_OPEN_SETTINGS);
  assert(flow.page == LAB_UI_PAGE_IDLE && !flow.picker_open);
  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.picker_open);
  cmd = action(LAB_UI_CMD_PICKER_NEW);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE);
  assert(flow.page == LAB_UI_PAGE_VOICE);
  lab_ui_flow_finish_voice(&flow, false);
  assert(flow.page == LAB_UI_PAGE_IDLE && flow.picker_open);

  cmd = action(LAB_UI_CMD_EXPERIMENT_SELECT);
  snprintf(cmd.experiment_id, sizeof(cmd.experiment_id), "READY-1");
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_FOCUS_EXPERIMENT);
  flow.picker_open = false;
  flow.page = LAB_UI_PAGE_RUNNING;
  ctx.have_view = true;
  ctx.last_step = false;

  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_TOGGLE_PAUSE);
  ctx.paused = true;
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_TOGGLE_PAUSE);
  ctx.paused = false;

  cmd = action(LAB_UI_CMD_SECONDARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_COMPLETE_STEP);

  cmd = action(LAB_UI_CMD_PARALLEL);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.page == LAB_UI_PAGE_TIMERS);
  cmd = action(LAB_UI_CMD_SECONDARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_FOCUS_NEXT);

  cmd = action(LAB_UI_CMD_VOICE);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_START_VOICE);
  assert(flow.page == LAB_UI_PAGE_VOICE);
  lab_ui_flow_finish_voice(&flow, true);
  assert(flow.page == LAB_UI_PAGE_TIMERS);

  lab_ui_flow_show_alert(&flow, LAB_UI_ALERT_ENVIRONMENT);
  assert(flow.page == LAB_UI_PAGE_ALERT);
  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_ACK_ENVIRONMENT);
  lab_ui_flow_finish_alert(&flow, true);
  assert(flow.page == LAB_UI_PAGE_TIMERS);

  /* A timer expiry must interrupt the conversation, then resume it after
   * acknowledgement rather than discarding the visible transcript. */
  flow.page = LAB_UI_PAGE_VOICE;
  lab_ui_flow_show_alert(&flow, LAB_UI_ALERT_TIMER_EXPIRED);
  assert(flow.page == LAB_UI_PAGE_ALERT);
  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.page == LAB_UI_PAGE_VOICE);
  assert(flow.alert_kind == LAB_UI_ALERT_NONE);
  lab_ui_flow_finish_voice(&flow, true);
  assert(flow.page == LAB_UI_PAGE_TIMERS);

  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.page == LAB_UI_PAGE_RUNNING);
  cmd = action(LAB_UI_CMD_TERTIARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.page == LAB_UI_PAGE_ALERT &&
         flow.alert_kind == LAB_UI_ALERT_CANCEL);
  cmd = action(LAB_UI_CMD_SECONDARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.page == LAB_UI_PAGE_RUNNING);
  cmd = action(LAB_UI_CMD_TERTIARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_CANCEL_EXPERIMENT);
  lab_ui_flow_finish_alert(&flow, false);
  assert(flow.page == LAB_UI_PAGE_IDLE);
  flow.page = LAB_UI_PAGE_RUNNING;
  ctx.last_step = true;
  cmd = action(LAB_UI_CMD_SECONDARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) == LAB_UI_EFFECT_NONE);
  assert(flow.alert_kind == LAB_UI_ALERT_COMPLETE);
  cmd = action(LAB_UI_CMD_PRIMARY);
  assert(lab_ui_flow_handle(&flow, &cmd, &ctx) ==
         LAB_UI_EFFECT_COMPLETE_EXPERIMENT);
  puts("lab_ui_flow_host_test: PASS");
  return 0;
}
