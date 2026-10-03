#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lab_ui_demo.h"
#include "lab_ui_flow.h"

typedef enum
{
  DEMO_READY = 0,
  DEMO_RUNNING,
  DEMO_PAUSED,
  DEMO_COMPLETED,
  DEMO_CANCELLED
} demo_state_t;

typedef struct
{
  const char *id;
  const char *name;
  demo_state_t state;
  uint8_t step;
  uint8_t steps;
  int32_t remaining;
} demo_experiment_t;

static lab_ui_t *g_ui;
static lab_ui_snapshot_t g_snapshot;
static lab_ui_flow_t g_flow;
static lv_timer_t *g_timer;
static lab_ui_demo_settings_cb_t g_settings_cb;
static void *g_settings_arg;
static uint32_t g_random_seed = 17;
static int g_focus;
static bool g_voice_pending;
#define DEMO_EXPERIMENT_COUNT 5

static const demo_experiment_t g_initial_experiments[DEMO_EXPERIMENT_COUNT] = {
  {"A-203", "酸碱滴定", DEMO_RUNNING, 4, 8, 291},
  {"B-117", "离心处理", DEMO_PAUSED, 2, 5, 131},
  {"C-052", "染色等待", DEMO_READY, 1, 4, 480},
  {"D-014", "样品恒温", DEMO_COMPLETED, 6, 6, 0},
  {"E-006", "缓冲液配制", DEMO_CANCELLED, 2, 4, 0},
};
static demo_experiment_t g_experiments[DEMO_EXPERIMENT_COUNT];

static size_t experiment_count(void)
{
  return DEMO_EXPERIMENT_COUNT;
}

static bool is_active(demo_state_t state)
{
  return state == DEMO_RUNNING || state == DEMO_PAUSED;
}

static const char *state_text(demo_state_t state)
{
  switch (state)
    {
      case DEMO_READY: return "待开始";
      case DEMO_RUNNING: return "运行中";
      case DEMO_PAUSED: return "已暂停";
      case DEMO_COMPLETED: return "已完成";
      default: return "已取消";
    }
}

static bool have_view(void)
{
  return g_focus >= 0 && (size_t)g_focus < experiment_count() &&
         is_active(g_experiments[g_focus].state);
}

static void focus_next_active(void)
{
  size_t offset;
  int start = g_focus < 0 ? 0 : g_focus + 1;
  for (offset = 0; offset < experiment_count(); offset++)
    {
      int index = (start + (int)offset) % (int)experiment_count();
      if (is_active(g_experiments[index].state))
        {
          g_focus = index;
          return;
        }
    }
  g_focus = -1;
}

static uint32_t next_random(void)
{
  g_random_seed = g_random_seed * 1103515245u + 12345u;
  return g_random_seed;
}

static void update_beijing_datetime(void)
{
  time_t now = time(NULL) + 8 * 3600;
  struct tm value;
#ifdef _WIN32
  gmtime_s(&value, &now);
#else
  gmtime_r(&now, &value);
#endif
  snprintf(g_snapshot.clock, sizeof(g_snapshot.clock), "%02d:%02d",
           value.tm_hour, value.tm_min);
  snprintf(g_snapshot.date, sizeof(g_snapshot.date),
           "%04d.%02d.%02d · UTC+8", value.tm_year + 1900,
           value.tm_mon + 1, value.tm_mday);
}

static void populate_lists(void)
{
  size_t i;
  int active_slot = 0;
  int recent_slot = 0;
  int timer_slot = 0;
  memset(g_snapshot.experiments, 0, sizeof(g_snapshot.experiments));
  memset(g_snapshot.recent, 0, sizeof(g_snapshot.recent));
  memset(g_snapshot.timers, 0, sizeof(g_snapshot.timers));
  for (i = 0; i < experiment_count(); i++)
    {
      demo_experiment_t *source = &g_experiments[i];
      if ((source->state == DEMO_READY || is_active(source->state)) &&
          active_slot < LAB_UI_EXPERIMENT_COUNT)
        {
          lab_ui_experiment_item_t *item =
            &g_snapshot.experiments[active_slot++];
          snprintf(item->experiment_id, sizeof(item->experiment_id), "%s",
                   source->id);
          snprintf(item->name, sizeof(item->name), "%s", source->name);
          snprintf(item->meta, sizeof(item->meta), "%s · 步骤 %u/%u",
                   state_text(source->state), source->step, source->steps);
          item->ready = source->state == DEMO_READY;
          item->paused = source->state == DEMO_PAUSED;
        }
      if ((source->state == DEMO_COMPLETED ||
           source->state == DEMO_CANCELLED) &&
          recent_slot < LAB_UI_RECENT_COUNT)
        {
          lab_ui_experiment_item_t *item = &g_snapshot.recent[recent_slot++];
          snprintf(item->experiment_id, sizeof(item->experiment_id), "%s",
                   source->id);
          snprintf(item->name, sizeof(item->name), "%s", source->name);
          snprintf(item->meta, sizeof(item->meta), "%s",
                   state_text(source->state));
        }
    }
  g_snapshot.experiment_count = (uint8_t)active_slot;
  g_snapshot.recent_count = (uint8_t)recent_slot;
  if (have_view())
    {
      demo_experiment_t *focus = &g_experiments[g_focus];
      snprintf(g_snapshot.timers[timer_slot].experiment,
               sizeof(g_snapshot.timers[timer_slot].experiment), "%s",
               focus->id);
      snprintf(g_snapshot.timers[timer_slot].task,
               sizeof(g_snapshot.timers[timer_slot].task), "%s",
               focus->name);
      g_snapshot.timers[timer_slot++].remaining_seconds = focus->remaining;
    }
  for (i = 0; i < experiment_count() && timer_slot < LAB_UI_TIMER_COUNT; i++)
    {
      demo_experiment_t *source = &g_experiments[i];
      if (!is_active(source->state) || (int)i == g_focus) continue;
      snprintf(g_snapshot.timers[timer_slot].experiment,
               sizeof(g_snapshot.timers[timer_slot].experiment), "%s",
               source->id);
      snprintf(g_snapshot.timers[timer_slot].task,
               sizeof(g_snapshot.timers[timer_slot].task), "%s",
               source->name);
      g_snapshot.timers[timer_slot++].remaining_seconds = source->remaining;
    }
}

static void populate_view(void)
{
  if (have_view())
    {
      demo_experiment_t *focus = &g_experiments[g_focus];
      snprintf(g_snapshot.experiment, sizeof(g_snapshot.experiment), "%s %s",
               focus->id, focus->name);
      snprintf(g_snapshot.step, sizeof(g_snapshot.step),
               "确认步骤 %u 并继续记录", focus->step);
      g_snapshot.step_index = focus->step;
      g_snapshot.step_count = focus->steps;
      g_snapshot.remaining_seconds = focus->remaining;
      g_snapshot.paused = focus->state == DEMO_PAUSED;
    }
  else
    {
      g_snapshot.experiment[0] = '\0';
      g_snapshot.step[0] = '\0';
      g_snapshot.step_index = 0;
      g_snapshot.step_count = 0;
      g_snapshot.remaining_seconds = 0;
      g_snapshot.paused = false;
    }
}

static void populate_alert(void)
{
  g_snapshot.alert_confirm_only = false;
  g_snapshot.alert_no_actions = false;
  g_snapshot.alert_id[0] = '\0';
  g_snapshot.alert_current[0] = '\0';
  g_snapshot.alert_threshold[0] = '\0';
  if (g_flow.alert_kind == LAB_UI_ALERT_ENVIRONMENT)
    {
      snprintf(g_snapshot.alert_title, sizeof(g_snapshot.alert_title),
               "温度环境告警");
      snprintf(g_snapshot.alert_id, sizeof(g_snapshot.alert_id),
               "env-20260813-0007 · temperature-high");
      snprintf(g_snapshot.alert_detail, sizeof(g_snapshot.alert_detail),
               "实验 A-203；事件已写入时间线");
      snprintf(g_snapshot.alert_current, sizeof(g_snapshot.alert_current),
               "31.2℃");
      snprintf(g_snapshot.alert_threshold,
               sizeof(g_snapshot.alert_threshold), "28.0℃");
      g_snapshot.alert_confirm_only = true;
    }
  else if (g_flow.alert_kind == LAB_UI_ALERT_COMPLETE)
    {
      snprintf(g_snapshot.alert_title, sizeof(g_snapshot.alert_title),
               "确认完成实验");
      snprintf(g_snapshot.alert_detail, sizeof(g_snapshot.alert_detail),
               "最后一步将标记完成，未结束计时器将停止。");
    }
  else if (g_flow.alert_kind == LAB_UI_ALERT_CANCEL)
    {
      snprintf(g_snapshot.alert_title, sizeof(g_snapshot.alert_title),
               "确认终止实验");
      snprintf(g_snapshot.alert_detail, sizeof(g_snapshot.alert_detail),
               "实验将记录为已取消，未完成计时器将停止。");
    }
}

static void refresh(void)
{
  if (!g_ui) return;
  update_beijing_datetime();
  populate_lists();
  populate_view();
  if (g_flow.page == LAB_UI_PAGE_ALERT) populate_alert();
  g_snapshot.page = g_flow.page;
  g_snapshot.experiment_picker_open = g_flow.picker_open;
  g_snapshot.voice_confirmation_pending = g_voice_pending;
  lab_ui_set_snapshot(g_ui, &g_snapshot);
}

static void tick_cb(lv_timer_t *timer)
{
  size_t i;
  (void)timer;
  for (i = 0; i < experiment_count(); i++)
    if (g_experiments[i].state == DEMO_RUNNING &&
        g_experiments[i].remaining > 0)
      g_experiments[i].remaining--;
  refresh();
}

static int find_experiment(const char *id)
{
  size_t i;
  for (i = 0; i < experiment_count(); i++)
    if (strcmp(g_experiments[i].id, id) == 0) return (int)i;
  return -1;
}

static void command_cb(const lab_ui_action_t *action, void *arg)
{
  lab_ui_flow_context_t context;
  lab_ui_effect_t effect;
  bool before_have_view;
  (void)arg;
  if (!action) return;
  memset(&context, 0, sizeof(context));
  before_have_view = have_view();
  context.have_view = before_have_view;
  if (before_have_view)
    {
      context.paused = g_experiments[g_focus].state == DEMO_PAUSED;
      context.last_step = g_experiments[g_focus].step >=
                          g_experiments[g_focus].steps;
    }
  context.voice_confirmation_pending = g_voice_pending;
  effect = lab_ui_flow_handle(&g_flow, action, &context);
  switch (effect)
    {
      case LAB_UI_EFFECT_OPEN_SETTINGS:
        if (g_settings_cb) g_settings_cb(g_settings_arg);
        return;
      case LAB_UI_EFFECT_START_VOICE:
      case LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE:
        g_voice_pending = true;
        snprintf(g_snapshot.voice_status, sizeof(g_snapshot.voice_status),
                 "正在聆听");
        snprintf(g_snapshot.voice_meta, sizeof(g_snapshot.voice_meta),
                 "置信度 92%%");
        snprintf(g_snapshot.transcript, sizeof(g_snapshot.transcript), "%s",
                 effect == LAB_UI_EFFECT_START_NEW_EXPERIMENT_VOICE ?
                 "你说：新建实验 F-021，分为四个步骤" :
                 "你说：A-203 的下一步加热到 50℃");
        break;
      case LAB_UI_EFFECT_FOCUS_EXPERIMENT:
        {
          int index = find_experiment(action->experiment_id);
          if (index >= 0 && (g_experiments[index].state == DEMO_READY ||
                            is_active(g_experiments[index].state)))
            {
              g_focus = index;
              if (g_experiments[index].state == DEMO_READY)
                g_experiments[index].state = DEMO_RUNNING;
              g_flow.picker_open = false;
              g_flow.page = LAB_UI_PAGE_RUNNING;
            }
        }
        break;
      case LAB_UI_EFFECT_TOGGLE_PAUSE:
        if (have_view())
          g_experiments[g_focus].state =
            g_experiments[g_focus].state == DEMO_PAUSED ?
            DEMO_RUNNING : DEMO_PAUSED;
        break;
      case LAB_UI_EFFECT_COMPLETE_STEP:
        if (have_view() && g_experiments[g_focus].step <
                           g_experiments[g_focus].steps)
          g_experiments[g_focus].step++;
        break;
      case LAB_UI_EFFECT_FOCUS_NEXT:
        focus_next_active();
        break;
      case LAB_UI_EFFECT_ACK_ENVIRONMENT:
        lab_ui_flow_finish_alert(&g_flow, have_view());
        break;
      case LAB_UI_EFFECT_COMPLETE_EXPERIMENT:
        if (have_view()) g_experiments[g_focus].state = DEMO_COMPLETED;
        focus_next_active();
        lab_ui_flow_finish_alert(&g_flow, have_view());
        break;
      case LAB_UI_EFFECT_CANCEL_EXPERIMENT:
        if (have_view()) g_experiments[g_focus].state = DEMO_CANCELLED;
        focus_next_active();
        lab_ui_flow_finish_alert(&g_flow, have_view());
        break;
      case LAB_UI_EFFECT_CANCEL_VOICE:
      case LAB_UI_EFFECT_CONFIRM_VOICE:
        g_voice_pending = false;
        lab_ui_flow_finish_voice(&g_flow, have_view());
        break;
      default:
        break;
    }
  refresh();
}

int lab_ui_demo_start(lab_ui_t *ui, lab_ui_demo_settings_cb_t settings_cb,
                      void *arg)
{
  if (!ui || g_ui) return -1;
  memset(&g_snapshot, 0, sizeof(g_snapshot));
  g_ui = ui;
  g_settings_cb = settings_cb;
  g_settings_arg = arg;
  memcpy(g_experiments, g_initial_experiments, sizeof(g_experiments));
  g_focus = 0;
  g_voice_pending = false;
  lab_ui_flow_init(&g_flow);
  g_snapshot.wifi_connected = true;
  g_snapshot.microphone_ready = true;
  snprintf(g_snapshot.weather, sizeof(g_snapshot.weather), "多云");
  snprintf(g_snapshot.weather_temperature,
           sizeof(g_snapshot.weather_temperature), "27℃");
  g_snapshot.weather_code = 2;
  snprintf(g_snapshot.environment, sizeof(g_snapshot.environment),
           "温度 24.5℃  湿度 60%%  距离 12.0厘米");
  snprintf(g_snapshot.temperature, sizeof(g_snapshot.temperature), "24.5℃");
  snprintf(g_snapshot.humidity, sizeof(g_snapshot.humidity), "60%%");
  snprintf(g_snapshot.distance, sizeof(g_snapshot.distance), "12.0厘米");
  snprintf(g_snapshot.voice_status, sizeof(g_snapshot.voice_status),
           "正在聆听");
  snprintf(g_snapshot.voice_meta, sizeof(g_snapshot.voice_meta),
           "置信度 92%%");
  snprintf(g_snapshot.transcript, sizeof(g_snapshot.transcript),
           "你说：A-203 的下一步加热到 50℃");
  lab_ui_flow_sync_content(&g_flow, have_view());
  g_timer = lv_timer_create(tick_cb, 1000, NULL);
  if (!g_timer)
    {
      g_ui = NULL;
      g_settings_cb = NULL;
      g_settings_arg = NULL;
      return -1;
    }
  refresh();
  return 0;
}

void lab_ui_demo_stop(void)
{
  if (g_timer) lv_timer_delete(g_timer);
  g_timer = NULL;
  g_ui = NULL;
  g_settings_cb = NULL;
  g_settings_arg = NULL;
}

void lab_ui_demo_set_page(lab_ui_page_t page)
{
  if (page > LAB_UI_PAGE_ALERT) return;
  if (page == LAB_UI_PAGE_VOICE)
    {
      lab_ui_action_t action = {.command = LAB_UI_CMD_VOICE};
      command_cb(&action, NULL);
      return;
    }
  if (page == LAB_UI_PAGE_ALERT)
    lab_ui_flow_show_alert(&g_flow, LAB_UI_ALERT_ENVIRONMENT);
  else
    {
      g_flow.picker_open = false;
      g_flow.page = page;
    }
  refresh();
}

void lab_ui_demo_set_scenario(const char *scenario)
{
  lab_ui_action_t action;
  if (!scenario) return;
  memset(&action, 0, sizeof(action));
  if (strcmp(scenario, "picker") == 0)
    {
      g_flow.page = LAB_UI_PAGE_IDLE;
      g_flow.picker_open = false;
      action.command = LAB_UI_CMD_PRIMARY;
      command_cb(&action, NULL);
    }
  else if (strcmp(scenario, "paused") == 0)
    {
      g_focus = 0;
      g_experiments[g_focus].state = DEMO_PAUSED;
      g_flow.picker_open = false;
      g_flow.page = LAB_UI_PAGE_RUNNING;
      refresh();
    }
  else if (strcmp(scenario, "end-confirm") == 0)
    {
      g_focus = 0;
      g_experiments[g_focus].state = DEMO_RUNNING;
      g_flow.picker_open = false;
      g_flow.page = LAB_UI_PAGE_RUNNING;
      action.command = LAB_UI_CMD_TERTIARY;
      command_cb(&action, NULL);
    }
  else if (strcmp(scenario, "complete-confirm") == 0)
    {
      g_focus = 0;
      g_experiments[g_focus].state = DEMO_RUNNING;
      g_experiments[g_focus].step = g_experiments[g_focus].steps;
      g_flow.picker_open = false;
      g_flow.page = LAB_UI_PAGE_RUNNING;
      action.command = LAB_UI_CMD_SECONDARY;
      command_cb(&action, NULL);
    }
}

void lab_ui_demo_randomize(void)
{
  unsigned temp = 230u + next_random() % 70u;
  unsigned humidity = 450u + next_random() % 400u;
  snprintf(g_snapshot.environment, sizeof(g_snapshot.environment),
           "温度 %u.%u℃  湿度 %u%%  距离 12.0厘米",
           temp / 10u, temp % 10u, humidity / 10u);
  refresh();
}

void lab_ui_demo_handle_command(const lab_ui_action_t *action, void *arg)
{
  command_cb(action, arg);
}
