#include "lab_ui.h"
#include <stdio.h>
#include <string.h>

/* Internal dock token; the visible microphone is drawn from LVGL primitives
 * so it does not depend on a private-use glyph in the selected CJK font. */
#define LAB_UI_MICROPHONE_ACTION "mic"

typedef enum
{
  DOCK_NORMAL = 0,
  DOCK_PRIMARY,
  DOCK_DANGER
} dock_style_t;

struct lab_ui_s
{
  lv_obj_t *root;
  lv_obj_t *pages[5];

  lv_obj_t *sbar_num;
  lv_obj_t *sbar_name;
  lv_obj_t *sbar_sub;
  lv_obj_t *wifi_led;
  lv_obj_t *mic_led;
  lv_obj_t *wifi_lbl;
  lv_obj_t *mic_lbl;
  lv_obj_t *mic_btn;
  lv_obj_t *stat_extra;
  lv_obj_t *settings_btn;

  lv_obj_t *idle_clock;
  lv_obj_t *idle_date;
  lv_obj_t *idle_weather_icon;
  lv_obj_t *idle_weather_sun;
  lv_obj_t *idle_weather_rain[3];
  lv_obj_t *idle_weather;
  lv_obj_t *idle_weather_temp;
  lv_obj_t *idle_env_temp;
  lv_obj_t *idle_env_humidity;
  lv_obj_t *idle_env_distance;
  lv_obj_t *idle_env_val; /* legacy producer compatibility */
  lv_obj_t *idle_recent_id[3];
  lv_obj_t *idle_recent_name[3];
  lv_obj_t *idle_recent_time[3];

  lv_obj_t *run_name;
  lv_obj_t *run_parallel_btn;
  lv_obj_t *run_pill_dot;
  lv_obj_t *run_pill_txt;
  lv_obj_t *run_clock;
  lv_obj_t *run_step_meta;
  lv_obj_t *run_step_progress;
  lv_obj_t *run_step_txt;
  lv_obj_t *run_step_bar;
  lv_obj_t *run_step_fill;
  lv_obj_t *run_temp;
  lv_obj_t *run_humidity;
  lv_obj_t *run_env_val;

  lv_obj_t *tm_focus_name;
  lv_obj_t *tm_focus_id;
  lv_obj_t *tm_focus_meta;
  lv_obj_t *tm_focus_big;
  lv_obj_t *tm_mini_num[2];
  lv_obj_t *tm_mini_name[2];
  lv_obj_t *tm_mini_sub[2];
  lv_obj_t *tm_mini_val[2];
  lv_obj_t *tm_mini_row[2];

  lv_obj_t *vc_status;
  lv_obj_t *vc_meta;
  lv_obj_t *vc_realtime;
  lv_obj_t *vc_transcript;

  lv_obj_t *al_title;
  lv_obj_t *al_id;
  lv_obj_t *al_now_val;
  lv_obj_t *al_thr_val;
  lv_obj_t *al_body;

  lv_obj_t *dock;
  lv_obj_t *dock_btns[3];
  lv_obj_t *dock_labels[3];
  lv_obj_t *dock_mic_icon;

  lv_obj_t *picker;
  lv_obj_t *picker_list;
  lv_obj_t *picker_rows[LAB_UI_EXPERIMENT_COUNT];
  lv_obj_t *picker_ids[LAB_UI_EXPERIMENT_COUNT];
  lv_obj_t *picker_names[LAB_UI_EXPERIMENT_COUNT];
  lv_obj_t *picker_meta[LAB_UI_EXPERIMENT_COUNT];
  lv_obj_t *picker_empty;
  lv_obj_t *picker_new_btn;
  lv_obj_t *picker_close_btn;

  lab_ui_fonts_t fonts;
  lab_ui_command_cb_t command_cb;
  void *command_arg;
  lab_ui_snapshot_t snapshot;
};

static const lv_font_t *safe_font(const lv_font_t *f)
{
  return f ? f : LV_FONT_DEFAULT;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color)
{
  return ui_label(parent, safe_font(font), color);
}

static void emit_action(lab_ui_t *ui, lab_ui_command_t command,
                        const char *experiment_id)
{
  lab_ui_action_t action;
  if (!ui || !ui->command_cb) return;
  memset(&action, 0, sizeof(action));
  action.command = command;
  if (experiment_id)
    snprintf(action.experiment_id, sizeof(action.experiment_id), "%s",
             experiment_id);
  ui->command_cb(&action, ui->command_arg);
}

/* Draw the settings affordance with LVGL primitives.  The R528 image uses a
 * compact CJK font which does not contain LV_SYMBOL_SETTINGS, so using the
 * symbol produces a missing-glyph square on the physical display. */
static lv_obj_t *make_settings_icon(lv_obj_t *parent, lv_color_t color)
{
  static const int8_t teeth[8][4] = {
    {8, 0, 4, 4}, {16, 8, 4, 4}, {8, 16, 4, 4}, {0, 8, 4, 4},
    {3, 3, 4, 4}, {13, 3, 4, 4}, {13, 13, 4, 4}, {3, 13, 4, 4}
  };
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_t *ring;
  lv_obj_t *hub;
  int i;

  lv_obj_set_size(icon, 20, 20);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  for (i = 0; i < 8; i++)
    {
      lv_obj_t *tooth = lv_obj_create(icon);
      lv_obj_set_size(tooth, teeth[i][2], teeth[i][3]);
      lv_obj_set_pos(tooth, teeth[i][0], teeth[i][1]);
      lv_obj_set_style_bg_color(tooth, color, 0);
      lv_obj_set_style_bg_opa(tooth, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(tooth, 0, 0);
      lv_obj_set_style_radius(tooth, 1, 0);
      lv_obj_clear_flag(tooth, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

  ring = lv_obj_create(icon);
  lv_obj_set_size(ring, 14, 14);
  lv_obj_set_pos(ring, 3, 3);
  lv_obj_set_style_bg_color(ring, ui_pal()->surface, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ring, 2, 0);
  lv_obj_set_style_border_color(ring, color, 0);
  lv_obj_set_style_radius(ring, 7, 0);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  hub = lv_obj_create(icon);
  lv_obj_set_size(hub, 4, 4);
  lv_obj_set_pos(hub, 8, 8);
  lv_obj_set_style_bg_color(hub, color, 0);
  lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hub, 0, 0);
  lv_obj_set_style_radius(hub, 2, 0);
  lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  return icon;
}

static lv_obj_t *make_mic_icon(lv_obj_t *parent, lv_color_t color)
{
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_t *frame;
  lv_obj_t *capsule;
  lv_obj_t *stem;
  lv_obj_t *base;

  lv_obj_set_size(icon, 18, 24);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  frame = lv_obj_create(icon);
  lv_obj_set_size(frame, 14, 16);
  lv_obj_set_pos(frame, 2, 1);
  lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(frame, 2, 0);
  lv_obj_set_style_border_side(frame,
                               LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT |
                               LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(frame, color, 0);
  lv_obj_set_style_radius(frame, 7, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  capsule = lv_obj_create(icon);
  lv_obj_set_size(capsule, 6, 11);
  lv_obj_set_pos(capsule, 6, 0);
  lv_obj_set_style_bg_color(capsule, color, 0);
  lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(capsule, 0, 0);
  lv_obj_set_style_radius(capsule, 3, 0);
  lv_obj_clear_flag(capsule, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  stem = lv_obj_create(icon);
  lv_obj_set_size(stem, 2, 5);
  lv_obj_set_pos(stem, 8, 16);
  lv_obj_set_style_bg_color(stem, color, 0);
  lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(stem, 0, 0);
  lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  base = lv_obj_create(icon);
  lv_obj_set_size(base, 12, 2);
  lv_obj_set_pos(base, 3, 21);
  lv_obj_set_style_bg_color(base, color, 0);
  lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(base, 0, 0);
  lv_obj_set_style_radius(base, 1, 0);
  lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return icon;
}

static void style_mic_icon(lab_ui_t *ui, lv_color_t color)
{
  int i;
  if (!ui->dock_mic_icon)
    {
      return;
    }
  for (i = 0; i < 4; i++)
    {
      lv_obj_t *part = lv_obj_get_child(ui->dock_mic_icon, i);
      if (!part)
        {
          continue;
        }
      lv_obj_set_style_bg_color(part, color, 0);
      lv_obj_set_style_border_color(part, color, 0);
    }
}

static lv_obj_t *weather_part(lv_obj_t *parent, int x, int y, int w, int h,
                              lv_color_t color, int radius)
{
  lv_obj_t *part = lv_obj_create(parent);
  lv_obj_set_size(part, w, h);
  lv_obj_set_pos(part, x, y);
  lv_obj_set_style_bg_color(part, color, 0);
  lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(part, 0, 0);
  lv_obj_set_style_radius(part, radius, 0);
  lv_obj_clear_flag(part, LV_OBJ_FLAG_SCROLLABLE);
  return part;
}

static lv_obj_t *make_weather_icon(lab_ui_t *ui, lv_obj_t *parent,
                                   const ui_palette_t *p)
{
  lv_obj_t *icon = lv_obj_create(parent);
  lv_obj_t *cloud;
  int i;
  lv_obj_set_size(icon, 28, 24);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_set_style_pad_all(icon, 0, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

  ui->idle_weather_sun = weather_part(icon, 15, 1, 9, 9, p->warn, 5);
  cloud = weather_part(icon, 3, 10, 22, 9, p->text_2, 5);
  (void)cloud;
  weather_part(icon, 8, 6, 11, 11, p->text_2, 6);
  ui->idle_weather_rain[0] = weather_part(icon, 7, 19, 2, 5, p->accent, 1);
  ui->idle_weather_rain[1] = weather_part(icon, 13, 19, 2, 5, p->accent, 1);
  ui->idle_weather_rain[2] = weather_part(icon, 19, 19, 2, 5, p->accent, 1);
  for (i = 0; i < 3; i++)
    {
      lv_obj_add_flag(ui->idle_weather_rain[i], LV_OBJ_FLAG_HIDDEN);
    }
  lv_obj_add_flag(ui->idle_weather_sun, LV_OBJ_FLAG_HIDDEN);
  return icon;
}

static void update_weather_icon(lab_ui_t *ui, uint8_t code)
{
  bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool sunny = code == 0;
  int i;
  if (sunny)
    lv_obj_clear_flag(ui->idle_weather_sun, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(ui->idle_weather_sun, LV_OBJ_FLAG_HIDDEN);
  for (i = 0; i < 3; i++)
    {
      if (rain)
        lv_obj_clear_flag(ui->idle_weather_rain[i], LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(ui->idle_weather_rain[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *make_page(lab_ui_t *ui)
{
  lv_obj_t *page = lv_obj_create(ui->root);
  ui_panel(page, ui_pal()->bg);
  lv_obj_set_size(page, 320, 158);
  lv_obj_set_pos(page, 0, 26);
  return page;
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
  lv_obj_t *c = lv_obj_create(parent);
  ui_card(c);
  lv_obj_set_size(c, w, h);
  lv_obj_set_pos(c, x, y);
  return c;
}

static void settings_btn_event(lv_event_t *e)
{
  lab_ui_t *ui = lv_event_get_user_data(e);
  LV_LOG_USER("Settings button clicked");
  if (ui && ui->command_cb)
    {
      emit_action(ui, LAB_UI_CMD_SETTINGS, NULL);
    }
}

static void voice_btn_event(lv_event_t *e)
{
  lab_ui_t *ui = lv_event_get_user_data(e);
  if (ui && ui->command_cb && ui->snapshot.microphone_ready)
    {
      emit_action(ui, LAB_UI_CMD_VOICE, NULL);
    }
}

static void parallel_btn_event(lv_event_t *e)
{
  lab_ui_t *ui = lv_event_get_user_data(e);
  emit_action(ui, LAB_UI_CMD_PARALLEL, NULL);
}

static void split_environment(const lab_ui_snapshot_t *snap,
                              char *temperature, size_t temperature_size,
                              char *humidity, size_t humidity_size,
                              char *distance, size_t distance_size)
{
  const char *p;
  snprintf(temperature, temperature_size, "%s",
           snap->temperature[0] ? snap->temperature : "--");
  snprintf(humidity, humidity_size, "%s",
           snap->humidity[0] ? snap->humidity : "--");
  snprintf(distance, distance_size, "%s",
           snap->distance[0] ? snap->distance : "--");
  if (!snap->environment[0]) return;
  p = strstr(snap->environment, "温度");
  if (!snap->temperature[0] && p) sscanf(p + strlen("温度"), "%15s", temperature);
  p = strstr(snap->environment, "湿度");
  if (!snap->humidity[0] && p) sscanf(p + strlen("湿度"), "%15s", humidity);
  p = strstr(snap->environment, "距离");
  if (!snap->distance[0] && p) sscanf(p + strlen("距离"), "%15s", distance);
}

/* ===== status bar ===== */
static void build_sbar(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *bar = lv_obj_create(ui->root);
  ui_panel(bar, p->surface);
  lv_obj_set_size(bar, 320, 26);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, p->border, 0);

  ui->sbar_num = make_label(bar, ui->fonts.caption, p->accent);
  lv_obj_set_pos(ui->sbar_num, 12, 8);
  lv_label_set_text(ui->sbar_num, "01");

  ui->sbar_name = make_label(bar, ui->fonts.body, p->text);
  lv_obj_set_pos(ui->sbar_name, 32, 6);

  ui->sbar_sub = make_label(bar, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->sbar_sub, 32, 15);
  lv_obj_add_flag(ui->sbar_sub, LV_OBJ_FLAG_HIDDEN);

  /* 右侧 MIC (最右) */
  ui->mic_lbl = make_label(bar, ui->fonts.caption, p->text_2);
  lv_obj_align(ui->mic_lbl, LV_ALIGN_RIGHT_MID, -34, 0);
  lv_label_set_text(ui->mic_lbl, "MIC");

  ui->mic_led = lv_obj_create(bar);
  lv_obj_set_size(ui->mic_led, 6, 6);
  lv_obj_align_to(ui->mic_led, ui->mic_lbl, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  lv_obj_set_style_radius(ui->mic_led, 3, 0);
  lv_obj_set_style_bg_opa(ui->mic_led, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->mic_led, 0, 0);
  lv_obj_clear_flag(ui->mic_led, LV_OBJ_FLAG_SCROLLABLE);

  /* A permanent push-to-talk target.  It remains available while an
   * experiment is running, even when the optional wake-word model is absent. */
  ui->mic_btn = lv_button_create(bar);
  lv_obj_set_size(ui->mic_btn, 54, 22);
  lv_obj_align(ui->mic_btn, LV_ALIGN_RIGHT_MID, -42, 0);
  lv_obj_set_style_bg_opa(ui->mic_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui->mic_btn, 0, 0);
  lv_obj_set_style_shadow_width(ui->mic_btn, 0, 0);
  lv_obj_set_style_pad_all(ui->mic_btn, 0, 0);
  lv_obj_add_event_cb(ui->mic_btn, voice_btn_event, LV_EVENT_RELEASED, ui);

  /* WIFI */
  ui->wifi_lbl = make_label(bar, ui->fonts.caption, p->text_2);
  lv_obj_align_to(ui->wifi_lbl, ui->mic_led, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  lv_label_set_text(ui->wifi_lbl, "WIFI");

  ui->wifi_led = lv_obj_create(bar);
  lv_obj_set_size(ui->wifi_led, 6, 6);
  lv_obj_align_to(ui->wifi_led, ui->wifi_lbl, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  lv_obj_set_style_radius(ui->wifi_led, 3, 0);
  lv_obj_set_style_bg_opa(ui->wifi_led, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->wifi_led, 0, 0);
  lv_obj_clear_flag(ui->wifi_led, LV_OBJ_FLAG_SCROLLABLE);

  /* Keep settings in the top-right status bar instead of the bottom dock. */
  ui->settings_btn = lv_button_create(ui->root);
  lv_obj_set_size(ui->settings_btn, 48, 44);
  lv_obj_align(ui->settings_btn, LV_ALIGN_TOP_RIGHT, -2, 2);
  lv_obj_set_style_bg_opa(ui->settings_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(ui->settings_btn, p->accent, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(ui->settings_btn, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ui->settings_btn, 0, 0);
  lv_obj_set_style_shadow_width(ui->settings_btn, 0, 0);
  lv_obj_set_style_radius(ui->settings_btn, 6, 0);
  lv_obj_set_style_pad_all(ui->settings_btn, 0, 0);
  lv_obj_add_flag(ui->settings_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(ui->settings_btn, settings_btn_event, LV_EVENT_CLICKED, ui);
  lv_obj_t *settings_icon = make_settings_icon(ui->settings_btn, p->text_2);
  lv_obj_align(settings_icon, LV_ALIGN_TOP_MID, 0, 1);

  /* TIMERS/ALERT 专用右侧状态 */
  ui->stat_extra = make_label(bar, ui->fonts.caption, p->text_2);
  lv_obj_align(ui->stat_extra, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_set_width(ui->stat_extra, 120);
  lv_obj_set_style_text_align(ui->stat_extra, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_add_flag(ui->stat_extra, LV_OBJ_FLAG_HIDDEN);

  /* Later status labels must never intercept the permanent settings target. */
  lv_obj_move_foreground(ui->settings_btn);
}

static void set_led(lv_obj_t *led, lv_color_t color, bool on)
{
  lv_obj_set_style_bg_color(led, on ? color : ui_pal()->text_3, 0);
}

static void set_sbar_right(lab_ui_t *ui, lab_ui_page_t page)
{
  const ui_palette_t *p = ui_pal();
  bool show_wifi_mic = true;
  char extra[40] = {0};

  switch (page)
    {
      case LAB_UI_PAGE_TIMERS:
        {
          int active = 0, expired = 0, i;
          for (i = 0; i < LAB_UI_TIMER_COUNT; i++)
            {
              if (ui->snapshot.timers[i].expired)
                expired++;
              else if (ui->snapshot.timers[i].remaining_seconds > 0)
                active++;
            }
          snprintf(extra, sizeof(extra), "%d 运行  %d 到期", active, expired);
          show_wifi_mic = false;
          break;
        }
      case LAB_UI_PAGE_ALERT:
        snprintf(extra, sizeof(extra), "! 严重");
        show_wifi_mic = false;
        break;
      default:
        break;
    }

  if (show_wifi_mic)
    {
      lv_obj_clear_flag(ui->wifi_led, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui->wifi_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui->mic_led, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui->mic_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui->mic_btn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui->stat_extra, LV_OBJ_FLAG_HIDDEN);
      set_led(ui->wifi_led, p->ok, ui->snapshot.wifi_connected);
      set_led(ui->mic_led, p->accent, ui->snapshot.microphone_ready);
      lv_obj_set_style_text_color(ui->wifi_lbl,
                                  ui->snapshot.wifi_connected ? p->text_2 : p->text_3, 0);
      lv_obj_set_style_text_color(ui->mic_lbl,
                                  ui->snapshot.microphone_ready ? p->text_2 : p->text_3, 0);
    }
  else
    {
      lv_obj_add_flag(ui->wifi_led, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui->wifi_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui->mic_led, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui->mic_lbl, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ui->mic_btn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui->stat_extra, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(ui->stat_extra, extra);
      lv_obj_set_style_text_color(ui->stat_extra,
                                  page == LAB_UI_PAGE_ALERT ? p->warn : p->text_2, 0);
    }
}

/* ===== dock ===== */
static void dock_btn_event(lv_event_t *e)
{
  lab_ui_t *ui = lv_event_get_user_data(e);
  lv_obj_t *target = lv_event_get_target(e);
  int i;
  if (!ui || !ui->command_cb)
    {
      return;
    }
  for (i = 0; i < 3; i++)
    {
      if (target == ui->dock_btns[i])
        {
          emit_action(ui, (lab_ui_command_t)i, NULL);
          return;
        }
    }
}

static void build_dock(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *dock = lv_obj_create(ui->root);
  lv_obj_set_style_bg_color(dock, p->surface, 0);
  lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dock, 1, 0);
  lv_obj_set_style_border_color(dock, p->border, 0);
  /* Do not inherit a second theme frame around the V7 capsule. */
  lv_obj_set_style_shadow_width(dock, 0, 0);
  lv_obj_set_style_outline_width(dock, 0, 0);
  lv_obj_set_style_outline_opa(dock, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(dock, 999, 0);
  lv_obj_set_style_pad_ver(dock, 4, 0);
  lv_obj_set_style_pad_hor(dock, 4, 0);
  lv_obj_set_style_pad_column(dock, 4, 0);
  lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(dock, LV_SIZE_CONTENT, 36);
  lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, -10);
  ui->dock = dock;

  int i;
  for (i = 0; i < 3; i++)
    {
      lv_obj_t *btn = lv_button_create(dock);
      lv_obj_set_height(btn, 28);
      lv_obj_set_width(btn, LV_SIZE_CONTENT);
      lv_obj_set_style_min_width(btn, 60, 0);
      lv_obj_set_style_pad_hor(btn, 14, 0);
      lv_obj_set_style_pad_ver(btn, 0, 0);
      lv_obj_set_style_radius(btn, 999, 0);
      lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(btn, 0, 0);
      lv_obj_set_style_shadow_width(btn, 0, 0);
      lv_obj_add_event_cb(btn, dock_btn_event, LV_EVENT_RELEASED, ui);
      lv_obj_t *lbl = make_label(btn, ui->fonts.label, p->text_2);
      lv_obj_center(lbl);
      ui->dock_btns[i] = btn;
      ui->dock_labels[i] = lbl;
    }
  ui->dock_mic_icon = make_mic_icon(ui->dock_btns[1], lv_color_hex(0xFFFFFF));
  lv_obj_center(ui->dock_mic_icon);
  lv_obj_add_flag(ui->dock_mic_icon, LV_OBJ_FLAG_HIDDEN);
}

static void picker_event(lv_event_t *e)
{
  lab_ui_t *ui = lv_event_get_user_data(e);
  lv_obj_t *target = lv_event_get_target(e);
  int i;
  if (!ui) return;
  if (target == ui->picker_close_btn)
    {
      emit_action(ui, LAB_UI_CMD_PICKER_CLOSE, NULL);
      return;
    }
  if (target == ui->picker_new_btn)
    {
      emit_action(ui, LAB_UI_CMD_PICKER_NEW, NULL);
      return;
    }
  for (i = 0; i < LAB_UI_EXPERIMENT_COUNT; i++)
    if (target == ui->picker_rows[i])
      {
        emit_action(ui, LAB_UI_CMD_EXPERIMENT_SELECT,
                    ui->snapshot.experiments[i].experiment_id);
        return;
      }
}

static lv_obj_t *picker_button(lab_ui_t *ui, lv_obj_t *parent,
                               const char *text, int width)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label = make_label(button, ui->fonts.label, p->text);
  lv_obj_set_size(button, width, 30);
  lv_obj_set_style_radius(button, 15, 0);
  lv_obj_set_style_bg_color(button, p->surface, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, p->border_soft, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_label_set_text(label, text);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(label);
  lv_obj_add_event_cb(button, picker_event, LV_EVENT_CLICKED, ui);
  return button;
}

static void build_experiment_picker(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *title;
  int i;
  ui->picker = lv_obj_create(ui->root);
  ui_panel(ui->picker, p->bg);
  lv_obj_set_size(ui->picker, 320, 240);
  lv_obj_set_pos(ui->picker, 0, 0);
  lv_obj_clear_flag(ui->picker, LV_OBJ_FLAG_SCROLLABLE);

  title = make_label(ui->picker, ui->fonts.body, p->text);
  lv_label_set_text(title, "选择实验");
  lv_obj_set_pos(title, 14, 8);

  ui->picker_list = lv_obj_create(ui->picker);
  ui_panel(ui->picker_list, p->bg);
  lv_obj_set_size(ui->picker_list, 304, 164);
  lv_obj_set_pos(ui->picker_list, 8, 32);
  lv_obj_set_style_pad_all(ui->picker_list, 4, 0);
  lv_obj_add_flag(ui->picker_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(ui->picker_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(ui->picker_list, LV_SCROLLBAR_MODE_AUTO);

  for (i = 0; i < LAB_UI_EXPERIMENT_COUNT; i++)
    {
      lv_obj_t *row = lv_button_create(ui->picker_list);
      lv_obj_set_size(row, 292, 42);
      lv_obj_set_pos(row, 0, i * 46);
      lv_obj_set_style_radius(row, 8, 0);
      lv_obj_set_style_bg_color(row, p->surface_2, 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, p->border_soft, 0);
      lv_obj_set_style_shadow_width(row, 0, 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(row, picker_event, LV_EVENT_CLICKED, ui);
      ui->picker_rows[i] = row;
      ui->picker_ids[i] = make_label(row, ui->fonts.caption, p->accent);
      lv_obj_set_pos(ui->picker_ids[i], 8, 5);
      ui->picker_names[i] = make_label(row, ui->fonts.small, p->text);
      lv_obj_set_pos(ui->picker_names[i], 8, 19);
      lv_obj_set_width(ui->picker_names[i], 188);
      lv_label_set_long_mode(ui->picker_names[i], LV_LABEL_LONG_DOT);
      ui->picker_meta[i] = make_label(row, ui->fonts.caption, p->text_2);
      lv_obj_set_width(ui->picker_meta[i], 82);
      lv_obj_set_style_text_align(ui->picker_meta[i], LV_TEXT_ALIGN_RIGHT, 0);
      lv_obj_align(ui->picker_meta[i], LV_ALIGN_RIGHT_MID, -8, 0);
      lv_obj_clear_flag(ui->picker_ids[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(ui->picker_names[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(ui->picker_meta[i], LV_OBJ_FLAG_CLICKABLE);
    }
  ui->picker_empty = make_label(ui->picker_list, ui->fonts.body, p->text_2);
  lv_label_set_text(ui->picker_empty, "暂无可开始或继续的实验");
  lv_obj_align(ui->picker_empty, LV_ALIGN_TOP_MID, 0, 46);

  ui->picker_new_btn = picker_button(ui, ui->picker, "语音新建实验", 176);
  lv_obj_set_pos(ui->picker_new_btn, 18, 202);
  ui->picker_close_btn = picker_button(ui, ui->picker, "返回", 96);
  lv_obj_set_pos(ui->picker_close_btn, 206, 202);
  lv_obj_add_flag(ui->picker, LV_OBJ_FLAG_HIDDEN);
}

static void style_dock_btn(lab_ui_t *ui, int idx, dock_style_t style)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *btn = ui->dock_btns[idx];
  lv_obj_t *lbl = ui->dock_labels[idx];
  if (style == DOCK_PRIMARY)
    {
      lv_obj_set_style_bg_color(btn, p->accent, 0);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    }
  else if (style == DOCK_DANGER)
    {
      lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lbl, p->err, 0);
    }
  else
    {
      lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_text_color(lbl, p->text_2, 0);
    }
}

static void set_dock(lab_ui_t *ui, const char *t0, dock_style_t s0,
                     const char *t1, dock_style_t s1,
                     const char *t2, dock_style_t s2)
{
  const char *texts[3] = {t0, t1, t2};
  dock_style_t styles[3] = {s0, s1, s2};
  int i;
  int count = 0;
  int visible = 0;
  int width;
  for (i = 0; i < 3; i++)
    if (texts[i] && texts[i][0]) count++;
  width = count == 3 ? 260 : (count == 2 ? 176 : (count == 1 ? 88 : 0));
  if (count)
    {
      lv_obj_set_size(ui->dock, width, 36);
      lv_obj_clear_flag(ui->dock, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_add_flag(ui->dock, LV_OBJ_FLAG_HIDDEN);
    }
  for (i = 0; i < 3; i++)
    {
      if (texts[i] && texts[i][0])
        {
          bool microphone = i == 1 &&
                            strcmp(texts[i], LAB_UI_MICROPHONE_ACTION) == 0;
          lv_label_set_text(ui->dock_labels[i], microphone ? "" : texts[i]);
          if (microphone)
            {
              lv_obj_add_flag(ui->dock_labels[i], LV_OBJ_FLAG_HIDDEN);
              lv_obj_clear_flag(ui->dock_mic_icon, LV_OBJ_FLAG_HIDDEN);
            }
          else
            {
              lv_obj_clear_flag(ui->dock_labels[i], LV_OBJ_FLAG_HIDDEN);
              if (i == 1)
                {
                  lv_obj_add_flag(ui->dock_mic_icon, LV_OBJ_FLAG_HIDDEN);
                }
            }
          style_dock_btn(ui, i, styles[i]);
          if (microphone)
            {
              style_mic_icon(ui, styles[i] == DOCK_PRIMARY ?
                             lv_color_hex(0xFFFFFF) : ui_pal()->text_2);
            }
          lv_obj_set_width(ui->dock_btns[i], 80);
          lv_obj_set_pos(ui->dock_btns[i], 4 + visible++ * 84, 4);
          lv_obj_clear_flag(ui->dock_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(ui->dock_btns[i], LV_OBJ_FLAG_HIDDEN);
          if (i == 1)
            {
              lv_obj_add_flag(ui->dock_mic_icon, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ===== IDLE ===== */
static void build_idle_page(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *page = make_page(ui);
  const char *names[3] = {"温度", "湿度", "距离"};
  ui->pages[LAB_UI_PAGE_IDLE] = page;
  ui->idle_clock = make_label(page, ui->fonts.mega, p->text);
  lv_obj_set_pos(ui->idle_clock, 14, 18);
  lv_label_set_text(ui->idle_clock, "--:--");
  ui->idle_date = make_label(page, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->idle_date, 14, 62);
  lv_label_set_text(ui->idle_date, "----.--.-- · UTC+8");
  ui->idle_weather_icon = make_weather_icon(ui, page, p);
  lv_obj_set_pos(ui->idle_weather_icon, 14, 78);
  ui->idle_weather = make_label(page, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->idle_weather, 46, 82);
  lv_label_set_text(ui->idle_weather, "多云");
  ui->idle_weather_temp = make_label(page, ui->fonts.small, p->text);
  lv_obj_set_pos(ui->idle_weather_temp, 82, 80);
  lv_obj_set_width(ui->idle_weather_temp, 66);
  lv_obj_set_style_text_align(ui->idle_weather_temp, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_text(ui->idle_weather_temp, "--");
  for (int i = 0; i < 3; i++)
    {
      lv_obj_t *card = make_card(page, 162, 8 + i * 28, 144, 24);
      lv_obj_t *key = make_label(card, ui->fonts.caption, p->text_2);
      lv_obj_t *value = make_label(card, ui->fonts.small, p->text);
      lv_obj_set_pos(key, 8, 5);
      lv_label_set_text(key, names[i]);
      lv_obj_set_pos(value, 62, 4);
      lv_obj_set_width(value, 72);
      lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
      lv_label_set_text(value, "--");
      if (i == 0) ui->idle_env_temp = value;
      else if (i == 1) ui->idle_env_humidity = value;
      else ui->idle_env_distance = value;
    }
  /* The whole idle page scrolls; the recent list may extend to the dock. */
  lv_obj_t *recent = make_card(page, 14, 106, 292, 76);
  lv_obj_t *ttl = make_label(recent, ui->fonts.caption, p->text_2);
  lv_obj_t *more = make_label(recent, ui->fonts.caption, p->accent);
  lv_label_set_text(ttl, "最近实验");
  lv_obj_set_pos(ttl, 10, 6);
  lv_label_set_text(more, "查看全部 >");
  lv_obj_align(more, LV_ALIGN_TOP_RIGHT, -10, 6);
  for (int i = 0; i < 3; i++)
    {
      lv_obj_t *dot = lv_obj_create(recent);
      lv_obj_set_size(dot, 5, 5);
      lv_obj_set_pos(dot, 12, 26 + i * 17);
      lv_obj_set_style_radius(dot, 3, 0);
      lv_obj_set_style_bg_color(dot, i == 0 ? p->accent : p->text_2, 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(dot, 0, 0);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
      ui->idle_recent_id[i] = make_label(recent, ui->fonts.caption,
                                         i == 0 ? p->accent : p->text_2);
      lv_obj_set_pos(ui->idle_recent_id[i], 22, 22 + i * 17);
      lv_label_set_text(ui->idle_recent_id[i], "-");
      ui->idle_recent_name[i] = make_label(recent, ui->fonts.small,
                                           i == 0 ? p->text : p->text_2);
      lv_obj_set_pos(ui->idle_recent_name[i], 66, 22 + i * 17);
      lv_obj_set_width(ui->idle_recent_name[i], 145);
      lv_label_set_long_mode(ui->idle_recent_name[i], LV_LABEL_LONG_DOT);
      lv_label_set_text(ui->idle_recent_name[i], "-");
      ui->idle_recent_time[i] = make_label(recent, ui->fonts.caption, p->text_2);
      lv_obj_align(ui->idle_recent_time[i], LV_ALIGN_TOP_RIGHT, -10,
                   22 + i * 17);
      lv_label_set_text(ui->idle_recent_time[i], "");
    }
  /* Scroll the complete idle body; the fixed dock remains the only overlay. */
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(page, 56, 0);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
}

/* ===== RUNNING ===== */
static void build_running_page(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *page = make_page(ui);
  ui->pages[LAB_UI_PAGE_RUNNING] = page;
  ui->run_name = make_label(page, ui->fonts.label, p->text);
  lv_obj_set_pos(ui->run_name, 14, 8);
  lv_obj_set_width(ui->run_name, 156);
  lv_label_set_long_mode(ui->run_name, LV_LABEL_LONG_DOT);
  ui->run_parallel_btn = lv_button_create(page);
  lv_obj_set_size(ui->run_parallel_btn, 48, 18);
  lv_obj_set_pos(ui->run_parallel_btn, 174, 8);
  lv_obj_set_style_radius(ui->run_parallel_btn, 9, 0);
  lv_obj_set_style_bg_opa(ui->run_parallel_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui->run_parallel_btn, 1, 0);
  lv_obj_set_style_border_color(ui->run_parallel_btn, p->border_soft, 0);
  lv_obj_set_style_shadow_width(ui->run_parallel_btn, 0, 0);
  lv_obj_clear_flag(ui->run_parallel_btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ui->run_parallel_btn, parallel_btn_event,
                      LV_EVENT_CLICKED, ui);
  {
    lv_obj_t *parallel_label = make_label(ui->run_parallel_btn,
                                          ui->fonts.caption, p->accent);
    lv_label_set_text(parallel_label, "并行");
    lv_obj_clear_flag(parallel_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(parallel_label);
  }
  lv_obj_t *pill = lv_obj_create(page);
  lv_obj_set_size(pill, 74, 18);
  lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -14, 8);
  lv_obj_set_style_radius(pill, 999, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(pill, 1, 0);
  lv_obj_set_style_border_color(pill, p->ok, 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  ui->run_pill_dot = lv_obj_create(pill);
  lv_obj_set_size(ui->run_pill_dot, 5, 5);
  lv_obj_set_pos(ui->run_pill_dot, 7, 6);
  lv_obj_set_style_radius(ui->run_pill_dot, 3, 0);
  lv_obj_set_style_bg_color(ui->run_pill_dot, p->ok, 0);
  lv_obj_set_style_bg_opa(ui->run_pill_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->run_pill_dot, 0, 0);
  lv_obj_clear_flag(ui->run_pill_dot, LV_OBJ_FLAG_SCROLLABLE);
  ui->run_pill_txt = make_label(pill, ui->fonts.caption, p->ok);
  lv_obj_align(ui->run_pill_txt, LV_ALIGN_LEFT_MID, 16, 0);
  ui->run_clock = make_label(page, ui->fonts.mega, p->text);
  lv_obj_set_pos(ui->run_clock, 0, 28);
  lv_obj_set_width(ui->run_clock, 320);
  lv_obj_set_style_text_align(ui->run_clock, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(ui->run_clock, "00:00");
  lv_obj_t *clbl = make_label(page, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(clbl, 0, 70);
  lv_obj_set_width(clbl, 320);
  lv_obj_set_style_text_align(clbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(clbl, "剩余  MIN : SEC");
  lv_obj_t *step = make_card(page, 14, 88, 292, 44);
  ui->run_step_meta = make_label(step, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->run_step_meta, 8, 4);
  ui->run_step_progress = make_label(step, ui->fonts.caption, p->text_2);
  lv_obj_set_width(ui->run_step_progress, 70);
  lv_obj_set_style_text_align(ui->run_step_progress, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(ui->run_step_progress, LV_ALIGN_TOP_RIGHT, -8, 4);
  ui->run_step_txt = make_label(step, ui->fonts.body, p->text);
  lv_obj_set_pos(ui->run_step_txt, 8, 17);
  lv_obj_set_width(ui->run_step_txt, 276);
  lv_label_set_long_mode(ui->run_step_txt, LV_LABEL_LONG_DOT);
  ui->run_step_bar = lv_obj_create(step);
  lv_obj_set_size(ui->run_step_bar, 274, 4);
  lv_obj_set_pos(ui->run_step_bar, 9, 34);
  lv_obj_set_style_bg_color(ui->run_step_bar, p->surface_3, 0);
  lv_obj_set_style_bg_opa(ui->run_step_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->run_step_bar, 0, 0);
  lv_obj_set_style_radius(ui->run_step_bar, 2, 0);
  lv_obj_clear_flag(ui->run_step_bar, LV_OBJ_FLAG_SCROLLABLE);
  ui->run_step_fill = lv_obj_create(ui->run_step_bar);
  lv_obj_set_size(ui->run_step_fill, 104, 4);
  lv_obj_set_style_bg_color(ui->run_step_fill, p->accent, 0);
  lv_obj_set_style_bg_opa(ui->run_step_fill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui->run_step_fill, 0, 0);
  lv_obj_set_style_radius(ui->run_step_fill, 2, 0);
  lv_obj_clear_flag(ui->run_step_fill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *env_temp = make_card(page, 14, 136, 142, 20);
  lv_obj_t *env_humidity = make_card(page, 164, 136, 142, 20);
  lv_obj_t *temp_key = make_label(env_temp, ui->fonts.caption, p->text_2);
  lv_obj_t *humidity_key = make_label(env_humidity, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(temp_key, 8, 3);
  lv_obj_set_pos(humidity_key, 8, 3);
  lv_label_set_text(temp_key, "温度");
  lv_label_set_text(humidity_key, "湿度");
  ui->run_temp = make_label(env_temp, ui->fonts.caption, p->text);
  ui->run_humidity = make_label(env_humidity, ui->fonts.caption, p->text);
  lv_obj_set_width(ui->run_temp, 92);
  lv_obj_set_width(ui->run_humidity, 92);
  lv_obj_set_style_text_align(ui->run_temp, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_align(ui->run_humidity, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(ui->run_temp, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_align(ui->run_humidity, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_label_set_text(ui->run_temp, "--");
  lv_label_set_text(ui->run_humidity, "--");
  ui->run_env_val = NULL;
}

/* ===== TIMERS ===== */
static void build_timers_page(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *page = make_page(ui);
  ui->pages[LAB_UI_PAGE_TIMERS] = page;

  lv_obj_t *focus = make_card(page, 14, 8, 292, 64);
  lv_obj_set_style_border_color(focus, p->accent, 0);
  ui->tm_focus_id = make_label(focus, ui->fonts.caption, p->accent);
  lv_obj_set_pos(ui->tm_focus_id, 12, 7);
  lv_label_set_text(ui->tm_focus_id, "- · 焦点");
  ui->tm_focus_name = make_label(focus, ui->fonts.body, p->text);
  lv_obj_set_pos(ui->tm_focus_name, 12, 19);
  lv_obj_set_width(ui->tm_focus_name, 170);
  lv_label_set_long_mode(ui->tm_focus_name, LV_LABEL_LONG_DOT);
  ui->tm_focus_meta = make_label(focus, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->tm_focus_meta, 12, 35);
  lv_obj_set_width(ui->tm_focus_meta, 170);
  lv_label_set_long_mode(ui->tm_focus_meta, LV_LABEL_LONG_DOT);
  ui->tm_focus_big = make_label(focus, ui->fonts.display, p->text);
  lv_obj_set_pos(ui->tm_focus_big, 180, 14);
  lv_obj_set_width(ui->tm_focus_big, 100);
  lv_obj_set_style_text_align(ui->tm_focus_big, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_text(ui->tm_focus_big, "00:00");

  for (int i = 0; i < 2; i++)
    {
      int y = 78 + i * 30;
      lv_obj_t *row = make_card(page, 14, y, 292, 26);
      ui->tm_mini_row[i] = row;
      ui->tm_mini_num[i] = make_label(row, ui->fonts.caption, p->text_2);
      lv_obj_set_pos(ui->tm_mini_num[i], 8, 6);
      ui->tm_mini_name[i] = make_label(row, ui->fonts.small, p->text);
      lv_obj_set_pos(ui->tm_mini_name[i], 28, 4);
      lv_obj_set_width(ui->tm_mini_name[i], 150);
      lv_label_set_long_mode(ui->tm_mini_name[i], LV_LABEL_LONG_DOT);
      ui->tm_mini_sub[i] = make_label(row, ui->fonts.caption, p->text_2);
      lv_obj_set_pos(ui->tm_mini_sub[i], 28, 15);
      lv_obj_set_width(ui->tm_mini_sub[i], 150);
      lv_label_set_long_mode(ui->tm_mini_sub[i], LV_LABEL_LONG_DOT);
      ui->tm_mini_val[i] = make_label(row, ui->fonts.label, p->text_2);
      lv_obj_set_pos(ui->tm_mini_val[i], 190, 4);
      lv_obj_set_width(ui->tm_mini_val[i], 94);
      lv_obj_set_style_text_align(ui->tm_mini_val[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
}

/* ===== VOICE ===== */
static void build_voice_page(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *page = make_page(ui);
  ui->pages[LAB_UI_PAGE_VOICE] = page;

  lv_obj_t *orb = lv_obj_create(page);
  lv_obj_set_size(orb, 30, 30);
  lv_obj_set_pos(orb, 14, 7);
  lv_obj_set_style_radius(orb, 15, 0);
  lv_obj_set_style_bg_color(orb, p->accent, 0);
  lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(orb, 0, 0);
  lv_obj_clear_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *voice_icon = make_mic_icon(orb, lv_color_hex(0xFFFFFF));
  lv_obj_center(voice_icon);

  ui->vc_status = make_label(page, ui->fonts.label, p->text);
  lv_obj_set_pos(ui->vc_status, 52, 6);
  lv_obj_set_width(ui->vc_status, 254);
  lv_label_set_long_mode(ui->vc_status, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui->vc_status, "正在聆听");

  ui->vc_meta = make_label(page, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->vc_meta, 52, 25);
  lv_obj_set_width(ui->vc_meta, 194);
  lv_label_set_long_mode(ui->vc_meta, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui->vc_meta, "等待语音");

  ui->vc_realtime = make_label(page, ui->fonts.caption, p->accent);
  lv_obj_set_width(ui->vc_realtime, 54);
  lv_obj_set_pos(ui->vc_realtime, 252, 25);
  lv_obj_set_style_text_align(ui->vc_realtime, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(ui->vc_realtime, LV_LABEL_LONG_DOT);
  lv_label_set_text(ui->vc_realtime, "实时");

  lv_obj_t *card = make_card(page, 14, 44, 292, 108);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(card, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
  ui->vc_transcript = make_label(card, ui->fonts.body, p->text);
  lv_obj_set_pos(ui->vc_transcript, 10, 8);
  lv_obj_set_width(ui->vc_transcript, 272);
  lv_label_set_long_mode(ui->vc_transcript, LV_LABEL_LONG_WRAP);
  lv_label_set_text(ui->vc_transcript, "");
}

/* ===== ALERT ===== */
static void build_alert_page(lab_ui_t *ui)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_t *page = make_page(ui);
  ui->pages[LAB_UI_PAGE_ALERT] = page;

  lv_obj_t *icon = lv_obj_create(page);
  lv_obj_set_size(icon, 36, 36);
  lv_obj_set_pos(icon, 14, 8);
  lv_obj_set_style_radius(icon, 18, 0);
  lv_obj_set_style_bg_color(icon, p->warn, 0);
  lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(icon, 0, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *bang = make_label(icon, ui->fonts.sub, lv_color_hex(0x4A3000));
  lv_obj_center(bang);
  lv_label_set_text(bang, "!");

  ui->al_title = make_label(page, ui->fonts.label, p->text);
  lv_obj_set_pos(ui->al_title, 58, 10);
  lv_obj_set_width(ui->al_title, 248);
  lv_label_set_long_mode(ui->al_title, LV_LABEL_LONG_DOT);
  ui->al_id = make_label(page, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(ui->al_id, 58, 28);
  lv_label_set_text(ui->al_id, "-");

  lv_obj_t *now_card = make_card(page, 14, 52, 142, 40);
  lv_obj_set_style_border_color(now_card, p->warn, 0);
  lv_obj_t *now_k = make_label(now_card, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(now_k, 10, 5);
  lv_label_set_text(now_k, "当前值");
  ui->al_now_val = make_label(now_card, ui->fonts.sub, p->warn);
  lv_obj_set_pos(ui->al_now_val, 10, 18);

  lv_obj_t *thr_card = make_card(page, 164, 52, 142, 40);
  lv_obj_t *thr_k = make_label(thr_card, ui->fonts.caption, p->text_2);
  lv_obj_set_pos(thr_k, 10, 5);
  lv_label_set_text(thr_k, "阈值");
  ui->al_thr_val = make_label(thr_card, ui->fonts.sub, p->text);
  lv_obj_set_pos(ui->al_thr_val, 10, 18);

  ui->al_body = make_label(page, ui->fonts.small, p->text_2);
  lv_obj_set_pos(ui->al_body, 14, 100);
  lv_obj_set_width(ui->al_body, 292);
  lv_label_set_long_mode(ui->al_body, LV_LABEL_LONG_WRAP);
}

static void fmt_countdown(char *buf, size_t size, int32_t sec)
{
  if (sec < 0)
    {
      snprintf(buf, size, "已到期");
      return;
    }
  snprintf(buf, size, "%02ld:%02ld", (long)(sec / 60), (long)(sec % 60));
}

static void render_experiment_picker(lab_ui_t *ui,
                                     const lab_ui_snapshot_t *snap)
{
  const ui_palette_t *p = ui_pal();
  int i;
  if (!snap->experiment_picker_open)
    {
      lv_obj_add_flag(ui->picker, LV_OBJ_FLAG_HIDDEN);
      return;
    }
  for (i = 0; i < LAB_UI_EXPERIMENT_COUNT; i++)
    {
      if (i < snap->experiment_count &&
          snap->experiments[i].experiment_id[0])
        {
          lv_label_set_text(ui->picker_ids[i],
                            snap->experiments[i].experiment_id);
          lv_label_set_text(ui->picker_names[i],
                            snap->experiments[i].name[0] ?
                            snap->experiments[i].name : "未命名实验");
          lv_label_set_text(ui->picker_meta[i],
                            snap->experiments[i].meta[0] ?
                            snap->experiments[i].meta : "可继续");
          lv_obj_set_style_border_color(ui->picker_rows[i],
                                        snap->experiments[i].ready ?
                                        p->accent : p->border_soft, 0);
          lv_obj_set_style_text_color(ui->picker_meta[i],
                                      snap->experiments[i].paused ?
                                      p->warn : p->text_2, 0);
          lv_obj_clear_flag(ui->picker_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(ui->picker_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
  if (snap->experiment_count)
    lv_obj_add_flag(ui->picker_empty, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_clear_flag(ui->picker_empty, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui->picker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ui->picker);
}

lab_ui_t *lab_ui_create(lv_obj_t *parent, const lab_ui_fonts_t *fonts,
                        lab_ui_command_cb_t command_cb, void *arg)
{
  lab_ui_t *ui;
  if (!parent)
    {
      return NULL;
    }
  ui = lv_malloc_zeroed(sizeof(*ui));
  if (!ui)
    {
      return NULL;
    }
  if (fonts)
    {
      ui->fonts = *fonts;
    }
  if (!ui_fonts())
    {
      ui_tokens_init(fonts);
    }
  ui->command_cb = command_cb;
  ui->command_arg = arg;

  ui->root = lv_obj_create(parent);
  ui_panel(ui->root, ui_pal()->bg);
  lv_obj_set_size(ui->root, 320, 240);
  lv_obj_set_pos(ui->root, 0, 0);

  build_sbar(ui);
  build_idle_page(ui);
  build_running_page(ui);
  build_timers_page(ui);
  build_voice_page(ui);
  build_alert_page(ui);
  build_dock(ui);
  build_experiment_picker(ui);

  /* Keep the top-right target above pages created after the status bar.  Its
   * lower half also avoids the panel's top-edge touch dead zone. */
  lv_obj_move_foreground(ui->settings_btn);

  memset(&ui->snapshot, 0, sizeof(ui->snapshot));
  lab_ui_set_snapshot(ui, &ui->snapshot);
  return ui;
}

void lab_ui_destroy(lab_ui_t *ui)
{
  if (!ui)
    {
      return;
    }
  if (ui->root)
    {
      lv_obj_delete(ui->root);
    }
  lv_free(ui);
}

void lab_ui_set_snapshot(lab_ui_t *ui, const lab_ui_snapshot_t *snap)
{
  const ui_palette_t *p;
  char buf[160];
  char cd[24];
  char temp[16];
  char humidity[16];
  char distance[16];
  int i;
  if (!ui || !snap || snap->page > LAB_UI_PAGE_ALERT)
    {
      return;
    }
  ui->snapshot = *snap;
  p = ui_pal();

  static const char *page_names[5] = {"待机", "运行", "并行", "语音", "告警"};
  static const char *page_nums[5] = {"01", "02", "03", "04", "05"};
  for (i = 0; i < 5; i++)
    {
      if ((lab_ui_page_t)i == snap->page)
        {
          lv_obj_clear_flag(ui->pages[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(ui->pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
  lv_label_set_text(ui->sbar_num, page_nums[snap->page]);
  lv_label_set_text(ui->sbar_name, page_names[snap->page]);
  if (snap->experiment[0] && snap->page == LAB_UI_PAGE_RUNNING)
    {
      const char *space = strchr(snap->experiment, ' ');
      if (space)
        snprintf(buf, sizeof(buf), "· %.*s", (int)(space - snap->experiment),
                 snap->experiment);
      else
        snprintf(buf, sizeof(buf), "· %s", snap->experiment);
      lv_label_set_text(ui->sbar_sub, buf);
      lv_obj_set_pos(ui->sbar_sub, 68, 6);
      lv_obj_clear_flag(ui->sbar_sub, LV_OBJ_FLAG_HIDDEN);
    }
  else
    {
      lv_obj_set_pos(ui->sbar_sub, 32, 15);
      lv_obj_add_flag(ui->sbar_sub, LV_OBJ_FLAG_HIDDEN);
    }
  set_sbar_right(ui, snap->page);
  split_environment(snap, temp, sizeof(temp), humidity, sizeof(humidity),
                    distance, sizeof(distance));

  /* Only update the visible page.  Hidden LVGL object trees are intentionally
   * left untouched until a page transition performs their full render. */
  if (snap->page == LAB_UI_PAGE_IDLE)
    {
  lv_label_set_text(ui->idle_clock,
                    snap->clock[0] ? snap->clock : "--:--");
  lv_label_set_text(ui->idle_date,
                    snap->date[0] ? snap->date : "----.--.-- · UTC+8");
  update_weather_icon(ui, snap->weather_code);
  lv_label_set_text(ui->idle_weather,
                    snap->weather[0] ? snap->weather : "待联网");
  lv_label_set_text(ui->idle_weather_temp,
                    snap->weather_temperature[0] ?
                      snap->weather_temperature : "--");
  lv_label_set_text(ui->idle_env_temp, temp);
  lv_label_set_text(ui->idle_env_humidity, humidity);
  lv_label_set_text(ui->idle_env_distance, distance);
  {
    for (i = 0; i < LAB_UI_RECENT_COUNT; i++)
      {
        lv_label_set_text(ui->idle_recent_id[i],
                          i < snap->recent_count &&
                          snap->recent[i].experiment_id[0] ?
                            snap->recent[i].experiment_id : "-");
        lv_label_set_text(ui->idle_recent_name[i],
                          i < snap->recent_count && snap->recent[i].name[0] ?
                            snap->recent[i].name : "-");
        lv_label_set_text(ui->idle_recent_time[i],
                          i < snap->recent_count ? snap->recent[i].meta : "");
        lv_obj_set_style_text_color(ui->idle_recent_id[i],
                                    i == 0 ? p->accent : p->text_2, 0);
        lv_obj_set_style_text_color(ui->idle_recent_name[i],
                                    i == 0 ? p->text : p->text_2, 0);
      }
  }
    }

  /* RUNNING */
  if (snap->page == LAB_UI_PAGE_RUNNING)
    {
  lv_label_set_text(ui->run_name,
                    snap->experiment[0] ? snap->experiment : "未命名实验");
  {
    const char *ptxt;
    lv_color_t pcol;
    if (snap->remaining_seconds < 0)
      {
        ptxt = "已到期";
        pcol = p->err;
      }
    else if (snap->paused)
      {
        ptxt = "已暂停";
        pcol = p->warn;
      }
    else
      {
        ptxt = "运行中";
        pcol = p->ok;
      }
    lv_label_set_text(ui->run_pill_txt, ptxt);
    lv_obj_set_style_text_color(ui->run_pill_txt, pcol, 0);
    lv_obj_set_style_border_color(ui->run_pill_txt->parent, pcol, 0);
    lv_obj_set_style_bg_color(ui->run_pill_dot, pcol, 0);
  }
  fmt_countdown(cd, sizeof(cd), snap->remaining_seconds);
  lv_label_set_text(ui->run_clock, cd);
  snprintf(buf, sizeof(buf), "步骤 %u / %u", snap->step_index,
           snap->step_count ? snap->step_count : 1);
  lv_label_set_text(ui->run_step_meta, buf);
  int progress = snap->step_count ?
                 (int)((snap->step_index * 100u) / snap->step_count) : 0;
  if (progress > 100) progress = 100;
  snprintf(buf, sizeof(buf), "%d%%", progress);
  lv_label_set_text(ui->run_step_progress, buf);
  lv_obj_set_width(ui->run_step_fill, (274 * progress) / 100);
  lv_label_set_text(ui->run_step_txt, snap->step[0] ? snap->step : "-");
  lv_label_set_text(ui->run_temp, temp);
  lv_label_set_text(ui->run_humidity, humidity);
    }

  /* TIMERS */
  if (snap->page == LAB_UI_PAGE_TIMERS)
  {
    int focus_idx = -1;
    int mini_idx = 0;
    for (i = 0; i < LAB_UI_TIMER_COUNT; i++)
      {
        if (!snap->timers[i].expired && focus_idx < 0)
          {
            focus_idx = i;
          }
      }
    if (focus_idx < 0)
      {
        focus_idx = 0;
      }
    snprintf(buf, sizeof(buf), "%s · 焦点",
             snap->timers[focus_idx].experiment[0] ?
               snap->timers[focus_idx].experiment : "-");
    lv_label_set_text(ui->tm_focus_id, buf);
    lv_label_set_text(ui->tm_focus_name,
                      snap->timers[focus_idx].task[0] ?
                        snap->timers[focus_idx].task : "-");
    snprintf(buf, sizeof(buf), "步骤 %u/%u · 进行中", snap->step_index,
             snap->step_count ? snap->step_count : 1);
    lv_label_set_text(ui->tm_focus_meta, buf);
    fmt_countdown(cd, sizeof(cd),
                  snap->timers[focus_idx].expired ? -1 :
                  snap->timers[focus_idx].remaining_seconds);
    lv_label_set_text(ui->tm_focus_big, cd);
    lv_obj_set_style_text_color(ui->tm_focus_big,
                                snap->timers[focus_idx].expired ? p->err : p->text, 0);
    for (i = 0; i < LAB_UI_TIMER_COUNT && mini_idx < 2; i++)
      {
        if (i == focus_idx)
          {
            continue;
          }
        snprintf(buf, sizeof(buf), "%d", i + 1);
        lv_label_set_text(ui->tm_mini_num[mini_idx], buf);
        lv_label_set_text(ui->tm_mini_name[mini_idx],
                          snap->timers[i].experiment[0] ?
                            snap->timers[i].experiment : "-");
        lv_label_set_text(ui->tm_mini_sub[mini_idx],
                          snap->timers[i].task[0] ? snap->timers[i].task : "-");
        if (snap->timers[i].expired)
          {
            lv_label_set_text(ui->tm_mini_val[mini_idx], "已到期");
            lv_obj_set_style_text_color(ui->tm_mini_val[mini_idx], p->err, 0);
            lv_obj_set_style_border_color(ui->tm_mini_row[mini_idx], p->err, 0);
          }
        else
          {
            fmt_countdown(cd, sizeof(cd), snap->timers[i].remaining_seconds);
            lv_label_set_text(ui->tm_mini_val[mini_idx], cd);
            lv_obj_set_style_text_color(ui->tm_mini_val[mini_idx], p->text_2, 0);
            lv_obj_set_style_border_color(ui->tm_mini_row[mini_idx],
                                          p->border_soft, 0);
          }
        mini_idx++;
      }
  }

  /* VOICE */
  if (snap->page == LAB_UI_PAGE_VOICE)
    {
  lv_label_set_text(ui->vc_status,
                    snap->voice_status[0] ? snap->voice_status : "语音未就绪");
  lv_label_set_text(ui->vc_meta,
                     snap->voice_meta[0] ? snap->voice_meta : "等待语音");
  lv_label_set_text(ui->vc_transcript,
                    snap->transcript[0] ? snap->transcript : "");
    }

  /* ALERT */
  if (snap->page == LAB_UI_PAGE_ALERT)
    {
  lv_label_set_text(ui->al_title,
                    snap->alert_title[0] ? snap->alert_title : "告警");
  lv_label_set_text(ui->al_id,
                     snap->alert_id[0] ? snap->alert_id : "-");
  lv_label_set_text(ui->al_body,
                    snap->alert_detail[0] ? snap->alert_detail : "");
  lv_label_set_text(ui->al_now_val,
                    snap->alert_current[0] ? snap->alert_current : "--");
  lv_label_set_text(ui->al_thr_val,
                    snap->alert_threshold[0] ? snap->alert_threshold : "--");
    }

  /* dock */
  switch (snap->page)
    {
      case LAB_UI_PAGE_IDLE:
        set_dock(ui, "开始实验", DOCK_NORMAL,
                 LAB_UI_MICROPHONE_ACTION, DOCK_PRIMARY,
                 "多任务", DOCK_NORMAL);
        break;
      case LAB_UI_PAGE_RUNNING:
        set_dock(ui, snap->paused ? "恢复" : "暂停", DOCK_NORMAL,
                 "下一步", DOCK_PRIMARY,
                 "结束", DOCK_DANGER);
        break;
      case LAB_UI_PAGE_TIMERS:
        set_dock(ui, "返回", DOCK_NORMAL,
                 "切换焦点", DOCK_PRIMARY,
                 "结束", DOCK_DANGER);
        break;
      case LAB_UI_PAGE_VOICE:
        if (snap->voice_confirmation_pending)
          {
            set_dock(ui, "取消", DOCK_DANGER,
                     "确认", DOCK_PRIMARY,
                     "", DOCK_NORMAL);
          }
        else
          {
            set_dock(ui, "返回", DOCK_NORMAL, "", DOCK_NORMAL,
                     "", DOCK_NORMAL);
          }
        break;
      case LAB_UI_PAGE_ALERT:
        if (snap->alert_no_actions)
          {
            set_dock(ui, "", DOCK_NORMAL, "", DOCK_NORMAL, "", DOCK_NORMAL);
          }
        else if (snap->alert_confirm_only)
          {
            set_dock(ui, "确认", DOCK_PRIMARY, "", DOCK_NORMAL, "", DOCK_NORMAL);
          }
        else
          {
            set_dock(ui, "确认", DOCK_PRIMARY, "返回", DOCK_NORMAL, "", DOCK_NORMAL);
          }
        break;
    }
  render_experiment_picker(ui, snap);
}
