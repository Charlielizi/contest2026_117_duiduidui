/****************************************************************************
 * Gemini-S1 launcher settings UI
 *
 * This module deliberately owns no display, input device, LVGL runtime, or
 * event loop.  luncher_mini owns those resources and supplies its screen.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <lvgl/lvgl.h>

#include "device_settings.h"
#include "device_settings_ui.h"
#include "ui_tokens.h"

#define SETTINGS_HEADER_HEIGHT 26
#define SETTINGS_ACTION_HEIGHT 36
#define SETTINGS_BOTTOM_GAP 56
#define SETTINGS_PORTAL_TIMEOUT_SEC 300U
#define SETTINGS_AUTO_PORTAL_DELAY_MS (30U * 1000U)
enum settings_view_e
{
  SETTINGS_VIEW_HOME = 0,
  SETTINGS_VIEW_WIFI,
  SETTINGS_VIEW_BLE,
  SETTINGS_VIEW_PORTAL,
  SETTINGS_VIEW_INFO,
};

enum settings_auto_portal_state_e
{
  SETTINGS_AUTO_PORTAL_WAITING = 0,
  SETTINGS_AUTO_PORTAL_CANCELLED,
  SETTINGS_AUTO_PORTAL_FIRED,
};

static lv_obj_t *g_screen;
static lv_obj_t *g_overlay;
static lv_obj_t *g_detail;
static lv_obj_t *g_list;
static lv_obj_t *g_actions[3];
static lv_obj_t *g_action_labels[3];
static lv_obj_t *g_action_bar;
static lv_obj_t *g_password_hint;
static lv_obj_t *g_password_dialog;
static lv_obj_t *g_qrcode;
static lv_timer_t *g_timer;
static const lv_font_t *g_font;
static int g_width;
static int g_height;
static int g_view;
static bool g_dirty;
static enum settings_auto_portal_state_e g_auto_portal_state;
static uint32_t g_auto_portal_deadline;
static char g_portal_qr_payload[128];
static pthread_mutex_t g_dirty_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_selected_ssid[DEVICE_SSID_MAX + 1];
static bool g_selected_secure;

static const ui_palette_t *settings_palette(void)
{
  return ui_pal();
}

static void settings_mark_dirty(void)
{
  pthread_mutex_lock(&g_dirty_lock);
  g_dirty = true;
  pthread_mutex_unlock(&g_dirty_lock);
}

static bool settings_take_dirty(void)
{
  bool dirty;
  pthread_mutex_lock(&g_dirty_lock);
  dirty = g_dirty;
  g_dirty = false;
  pthread_mutex_unlock(&g_dirty_lock);
  return dirty;
}

/* Gemini-S1 has a 320x240 display.  The stock LVGL keyboard makes the
 * numeric keys too small, so password entry starts with a 3x4 number pad.
 * The alternate page exposes all 26 lower-case English letters. */
static const char *g_password_number_map[] =
{
  "1", "2", "3", "\n",
  "4", "5", "6", "\n",
  "7", "8", "9", "\n",
  "ABC", "0", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};

static const lv_buttonmatrix_ctrl_t g_password_number_ctrl[] =
{
  1, 1, 1,
  1, 1, 1,
  1, 1, 1,
  1, 1, 1, 1
};

static const char *g_password_alpha_map[] =
{
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
  "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
  "123", " ", ".", LV_SYMBOL_OK, NULL
};

static const lv_buttonmatrix_ctrl_t g_password_alpha_ctrl[] =
{
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 2,
  2, 4, 2, 2
};

static const lv_font_t *settings_font(void)
{
  return g_font ? g_font : LV_FONT_DEFAULT;
}

static void settings_clear_button_effects(lv_obj_t *button)
{
  static const lv_state_t states[] = {
    LV_STATE_DEFAULT, LV_STATE_PRESSED, LV_STATE_FOCUSED,
    LV_STATE_FOCUS_KEY, LV_STATE_CHECKED, LV_STATE_DISABLED
  };
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++)
    {
      lv_obj_set_style_outline_width(button, 0, states[i]);
      lv_obj_set_style_outline_opa(button, LV_OPA_TRANSP, states[i]);
      lv_obj_set_style_shadow_width(button, 0, states[i]);
      lv_obj_set_style_shadow_opa(button, LV_OPA_TRANSP, states[i]);
    }
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *settings_button(lv_obj_t *parent, const char *text,
                                 int width, int height)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_t *label = lv_label_create(button);

  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_style_bg_color(button, p->surface, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, p->border_soft, 0);
  settings_clear_button_effects(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, settings_font(), 0);
  lv_obj_set_style_text_color(label, p->text, 0);
  lv_obj_center(label);
  return button;
}

static const char *wifi_state_text(device_wifi_state_t state)
{
  switch (state)
    {
      case DEVICE_WIFI_SCANNING:
        return "正在扫描";
      case DEVICE_WIFI_ASSOCIATING:
        return "正在连接";
      case DEVICE_WIFI_DHCP:
        return "正在获取IP";
      case DEVICE_WIFI_ONLINE:
        return "已联网";
      case DEVICE_WIFI_ERROR:
        return "连接失败";
      default:
        return "离线";
    }
}

static void settings_service_event(device_settings_event_t event, void *arg)
{
  (void)event;
  (void)arg;
  /* The worker only marks state dirty.  LVGL objects are refreshed by the
   * launcher-owned LVGL timer, never from this callback. */
  settings_mark_dirty();
}

static void settings_close_cb(lv_event_t *e)
{
  (void)e;
  if (g_view == SETTINGS_VIEW_PORTAL)
    device_wifi_portal_close();
  if (g_overlay)
    {
      lv_obj_delete(g_overlay);
      g_overlay = NULL;
      g_detail = NULL;
      g_list = NULL;
      g_action_bar = NULL;
      g_password_dialog = NULL;
      g_qrcode = NULL;
    }

  g_view = SETTINGS_VIEW_HOME;
}

static void settings_relayout_actions(void)
{
  int count = 0;
  int visible = 0;
  int width;
  for (int i = 0; i < 3; i++)
    if (g_actions[i] && !lv_obj_has_flag(g_actions[i], LV_OBJ_FLAG_HIDDEN)) count++;
  if (!g_action_bar) return;
  width = count == 3 ? 260 : (count == 2 ? 176 : (count == 1 ? 88 : 0));
  if (!count)
    {
      lv_obj_add_flag(g_action_bar, LV_OBJ_FLAG_HIDDEN);
      return;
    }
  lv_obj_set_size(g_action_bar, width, SETTINGS_ACTION_HEIGHT);
  lv_obj_set_pos(g_action_bar, (g_width - width) / 2,
                 g_height - SETTINGS_ACTION_HEIGHT - 10);
  lv_obj_clear_flag(g_action_bar, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 3; i++)
    if (g_actions[i] && !lv_obj_has_flag(g_actions[i], LV_OBJ_FLAG_HIDDEN))
      {
        lv_obj_set_size(g_actions[i], 80, 28);
        lv_obj_set_pos(g_actions[i], 4 + visible++ * 84, 4);
      }
}

static void settings_action_config(int index, const char *text, lv_event_cb_t cb)
{
  const ui_palette_t *p = settings_palette();
  bool primary = text && (strcmp(text, "选择") == 0 || strcmp(text, "扫描") == 0 ||
                          strcmp(text, "开启热点") == 0 || strcmp(text, "续期 5m") == 0);
  if (index < 0 || index >= 3 || !g_actions[index]) return;
  lv_label_set_text(g_action_labels[index], text ? text : "");
  lv_obj_set_style_bg_opa(g_actions[index], primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(g_actions[index], p->accent, 0);
  lv_obj_set_style_border_width(g_actions[index], 0, 0);
  lv_obj_set_style_text_color(g_action_labels[index], primary ?
                              lv_color_hex(0xFFFFFF) : p->text_2, 0);
  if (text && text[0])
    {
      lv_obj_clear_flag(g_actions[index], LV_OBJ_FLAG_HIDDEN);
      if (cb) lv_obj_add_event_cb(g_actions[index], cb, LV_EVENT_CLICKED, NULL);
    }
  else
    lv_obj_add_flag(g_actions[index], LV_OBJ_FLAG_HIDDEN);
  settings_relayout_actions();
}

static void settings_new_overlay(const char *title)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *header;
  lv_obj_t *label;
  lv_obj_t *status;
  const char *status_text = "● WIFI";
  lv_color_t status_color = p->ok;
  if (strcmp(title, "Wi-Fi") == 0) status_text = "● 已联网";
  else if (strcmp(title, "蓝牙") == 0) status_text = "● 扫描中";
  else if (strcmp(title, "扫码配网") == 0) { status_text = "● 开启中"; status_color = p->accent; }
  else if (strcmp(title, "信息") == 0) { status_text = "openvela"; status_color = p->text_2; }
  if (g_overlay) lv_obj_delete(g_overlay);
  g_action_bar = NULL;
  g_overlay = lv_obj_create(g_screen);
  lv_obj_set_size(g_overlay, g_width, g_height);
  lv_obj_set_pos(g_overlay, 0, 0);
  ui_panel(g_overlay, p->bg);
  header = lv_obj_create(g_overlay);
  ui_panel(header, p->surface);
  lv_obj_set_size(header, g_width, SETTINGS_HEADER_HEIGHT);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_border_width(header, 1, 0);
  lv_obj_set_style_border_color(header, p->border, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
  label = lv_label_create(header);
  lv_label_set_text(label, title);
  lv_obj_set_style_text_font(label, settings_font(), 0);
  lv_obj_set_style_text_color(label, p->text, 0);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
  status = lv_label_create(header);
  lv_label_set_text(status, status_text);
  lv_obj_set_style_text_font(status, settings_font(), 0);
  lv_obj_set_style_text_color(status, status_color, 0);
  lv_obj_align(status, LV_ALIGN_RIGHT_MID, -12, 0);
  g_action_bar = lv_obj_create(g_overlay);
  ui_panel(g_action_bar, p->surface);
  lv_obj_set_size(g_action_bar, 88, SETTINGS_ACTION_HEIGHT);
  lv_obj_set_pos(g_action_bar, (g_width - 88) / 2,
                 g_height - SETTINGS_ACTION_HEIGHT - 10);
  lv_obj_set_style_border_width(g_action_bar, 1, 0);
  lv_obj_set_style_border_color(g_action_bar, p->border, 0);
  lv_obj_set_style_radius(g_action_bar, 999, 0);
  lv_obj_set_style_shadow_width(g_action_bar, 0, 0);
  lv_obj_set_style_outline_width(g_action_bar, 0, 0);
  lv_obj_set_style_outline_opa(g_action_bar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(g_action_bar, 4, 0);
  for (int i = 0; i < 3; i++)
    {
      g_actions[i] = lv_button_create(g_action_bar);
      lv_obj_set_size(g_actions[i], 80, 28);
      lv_obj_set_pos(g_actions[i], 4 + i * 84, 4);
      lv_obj_set_style_radius(g_actions[i], 999, 0);
      settings_clear_button_effects(g_actions[i]);
      g_action_labels[i] = lv_label_create(g_actions[i]);
      lv_obj_set_style_text_font(g_action_labels[i], settings_font(), 0);
      lv_obj_center(g_action_labels[i]);
      lv_obj_add_flag(g_actions[i], LV_OBJ_FLAG_HIDDEN);
    }
  g_detail = NULL;
  g_list = NULL;
  g_qrcode = NULL;
}

static void password_close_cb(lv_event_t *e)
{
  lv_obj_t *dialog = e ? lv_event_get_user_data(e) : g_password_dialog;
  if (dialog)
    {
      g_password_hint = NULL;
      g_password_dialog = NULL;
      lv_obj_delete(dialog);
      settings_action_config(0, "", NULL);
      settings_action_config(1, "选择", NULL);
      settings_action_config(2, "返回", settings_close_cb);
    }
}

static void password_action_cb(lv_event_t *e)
{
  (void)e;
  password_close_cb(NULL);
}

static void password_keyboard_layout_cb(lv_event_t *e)
{
  lv_obj_t *keyboard = lv_event_get_target(e);
  uint32_t button = lv_buttonmatrix_get_selected_button(keyboard);
  const char *text;

  if (button == LV_BUTTONMATRIX_BUTTON_NONE)
    {
      return;
    }

  text = lv_buttonmatrix_get_button_text(keyboard, button);
  if (!text)
    {
      return;
    }

  if (strcmp(text, "ABC") == 0)
    {
      lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_2);
    }
  else if (strcmp(text, "123") == 0)
    {
      /* The default keyboard handler inserts the text before this callback.
       * Remove that temporary marker, then return to the large number pad. */
      lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard);
      if (textarea)
        {
          lv_textarea_delete_char(textarea);
          lv_textarea_delete_char(textarea);
          lv_textarea_delete_char(textarea);
        }
      lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    }
}

static void password_ready_cb(lv_event_t *e)
{
  lv_obj_t *keyboard = lv_event_get_target(e);
  lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard);
  const char *password = lv_textarea_get_text(textarea);
  int ret = device_wifi_connect(g_selected_ssid,
                                g_selected_secure ? password : "");

  if (ret < 0 && g_detail)
    {
      /* Do not hide validation feedback behind the full-screen dialog. */
      if (g_password_hint)
        {
          lv_label_set_text(g_password_hint, "密码需 8-63 个字符");
        }
    }
  else
    {
      g_password_dialog = lv_obj_get_parent(keyboard);
      password_close_cb(NULL);
    }
}

static void show_password_dialog(void)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *dialog = lv_obj_create(g_overlay);
  lv_obj_t *textarea;
  lv_obj_t *keyboard;
  ui_panel(dialog, p->bg);
  lv_obj_set_size(dialog, g_width, g_height - SETTINGS_HEADER_HEIGHT - SETTINGS_BOTTOM_GAP);
  lv_obj_set_pos(dialog, 0, SETTINGS_HEADER_HEIGHT);
  textarea = lv_textarea_create(dialog);
  lv_obj_set_size(textarea, 292, 28);
  lv_obj_set_pos(textarea, 14, 6);
  lv_textarea_set_one_line(textarea, true);
  lv_textarea_set_password_mode(textarea, true);
  lv_textarea_set_placeholder_text(textarea, "请输入 Wi-Fi 密码");
  lv_textarea_set_max_length(textarea, 63);
  lv_obj_set_style_bg_color(textarea, p->surface, 0);
  lv_obj_set_style_border_width(textarea, 1, 0);
  lv_obj_set_style_border_color(textarea, p->border_soft, 0);
  lv_obj_set_style_text_color(textarea, p->text, 0);
  lv_obj_set_style_text_font(textarea, settings_font(), 0);
  g_password_hint = lv_label_create(dialog);
  lv_obj_set_width(g_password_hint, 292);
  lv_obj_set_pos(g_password_hint, 14, 36);
  lv_label_set_text(g_password_hint, "请输入 8-63 位密码");
  lv_obj_set_style_text_font(g_password_hint, settings_font(), 0);
  lv_obj_set_style_text_color(g_password_hint, p->text_2, 0);
  keyboard = lv_keyboard_create(dialog);
  lv_obj_set_size(keyboard, 292, 96);
  lv_obj_set_pos(keyboard, 14, 54);
  lv_obj_set_style_bg_color(keyboard, p->surface_2, 0);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(keyboard, 1, 0);
  lv_obj_set_style_border_color(keyboard, p->border_soft, 0);
  lv_obj_set_style_bg_color(keyboard, p->surface_2, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, p->text, LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(keyboard, p->border_soft, LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, 6, 0);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1,
                      g_password_number_map, g_password_number_ctrl);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_2,
                      g_password_alpha_map, g_password_alpha_ctrl);
  lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
  lv_keyboard_set_textarea(keyboard, textarea);
  lv_obj_add_event_cb(keyboard, password_keyboard_layout_cb,
                      LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(keyboard, password_ready_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(keyboard, password_close_cb, LV_EVENT_CANCEL, dialog);
  lv_obj_set_align(keyboard, LV_ALIGN_DEFAULT);
  lv_obj_set_pos(keyboard, 14, 54);
  g_password_dialog = dialog;
  settings_action_config(0, "取消", password_action_cb);
  settings_action_config(1, "", NULL);
  settings_action_config(2, "", NULL);
}

static void wifi_ap_cb(lv_event_t *e)
{
  uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
  device_wifi_status_t status;

  if (device_wifi_get_status(&status) < 0 || index >= status.ap_count)
    {
      return;
    }

  snprintf(g_selected_ssid, sizeof(g_selected_ssid), "%s",
           status.aps[index].ssid);
  g_selected_secure = status.aps[index].secure;
  if (g_selected_secure)
    {
      show_password_dialog();
    }
  else
    {
      device_wifi_connect(g_selected_ssid, "");
    }
}

static void wifi_scan_cb(lv_event_t *e)
{
  (void)e;
  device_wifi_scan();
}

static void wifi_forget_cb(lv_event_t *e)
{
  (void)e;
  device_wifi_forget();
}

static void refresh_wifi(void)
{
  device_wifi_status_t status;
  char text[96];
  size_t i;

  if (!g_overlay || !g_list || device_wifi_get_status(&status) < 0)
    {
      return;
    }

  snprintf(text, sizeof(text), "%s\n%s · %ddBm",
           status.ssid[0] ? status.ssid : "--",
           status.ip[0] ? status.ip : "0.0.0.0", status.rssi);
  lv_label_set_text(g_detail, text);
  lv_obj_clean(g_list);

  for (i = 0; i < status.ap_count; i++)
    {
      char row[72];
      lv_obj_t *button;
      snprintf(row, sizeof(row), "%s  %s  %ddBm",
               status.aps[i].secure ? "加密" : "开放",
               status.aps[i].ssid, status.aps[i].rssi);
      button = settings_button(g_list, row, 286, 20);
      lv_obj_add_event_cb(button, wifi_ap_cb, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)i);
    }

  if (status.ap_count == 0)
    {
      lv_obj_t *empty = lv_label_create(g_list);
      lv_label_set_text(empty, status.state == DEVICE_WIFI_SCANNING ?
                        "正在扫描热点..." : "点击扫描查找 Wi-Fi");
      lv_obj_set_style_text_font(empty, settings_font(), 0);
      lv_obj_set_style_text_color(empty, settings_palette()->text, 0);
    }
}

static void show_wifi_cb(lv_event_t *e)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *card;
  lv_obj_t *scan;
  lv_obj_t *forget;
  (void)e;
  g_view = SETTINGS_VIEW_WIFI;
  settings_new_overlay("Wi-Fi");
  card = lv_obj_create(g_overlay);
  ui_card(card);
  lv_obj_set_size(card, 292, 34);
  lv_obj_set_pos(card, 14, 34);
  g_detail = lv_label_create(g_overlay);
  lv_obj_set_width(g_detail, 156);
  lv_obj_set_pos(g_detail, 22, 39);
  lv_obj_set_style_text_font(g_detail, settings_font(), 0);
  lv_obj_set_style_text_color(g_detail, p->text, 0);
  scan = settings_button(card, "扫描", 48, 26);
  lv_obj_set_pos(scan, 172, 4);
  lv_obj_add_event_cb(scan, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
  forget = settings_button(card, "忘记", 48, 26);
  lv_obj_set_pos(forget, 226, 4);
  lv_obj_add_event_cb(forget, wifi_forget_cb, LV_EVENT_CLICKED, NULL);
  g_list = lv_obj_create(g_overlay);
  ui_panel(g_list, p->bg);
  lv_obj_set_size(g_list, 292, 116);
  lv_obj_set_pos(g_list, 14, 74);
  lv_obj_set_style_pad_all(g_list, 2, 0);
  lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g_list, 3, 0);
  lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
  refresh_wifi();
  device_wifi_scan();
  settings_action_config(1, "选择", NULL);
  settings_action_config(2, "返回", settings_close_cb);
}

static void ble_device_cb(lv_event_t *e)
{
  uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
  device_ble_status_t status;

  if (device_ble_get_status(&status) < 0 || index >= status.device_count)
    {
      return;
    }

  if (status.devices[index].bonded)
    {
      device_ble_unpair(status.devices[index].address);
    }
  else
    {
      device_ble_pair(status.devices[index].address);
    }
}

static void ble_scan_cb(lv_event_t *e)
{
  (void)e;
  device_ble_set_enabled(true);
  device_ble_scan();
}

static void refresh_ble(void)
{
  device_ble_status_t status;
  size_t i;

  if (!g_overlay || !g_list || device_ble_get_status(&status) < 0)
    {
      return;
    }

  lv_label_set_text(g_detail,
                    status.state == DEVICE_BLE_SCANNING ? "正在扫描蓝牙设备..." :
                    status.state == DEVICE_BLE_PAIRING ? "正在配对..." :
                    status.state == DEVICE_BLE_ERROR ? "蓝牙操作失败" :
                    "点击设备配对/解绑    3 已配对 · 5 发现");
  lv_obj_clean(g_list);
  for (i = 0; i < status.device_count; i++)
    {
      char row[80];
      lv_obj_t *button;
      snprintf(row, sizeof(row), "%s%s  %ddBm",
               status.devices[i].bonded ? "已配对 " : "",
               status.devices[i].name, status.devices[i].rssi);
      button = settings_button(g_list, row, 286, 20);
      lv_obj_add_event_cb(button, ble_device_cb, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)i);
    }
}

static void show_ble_cb(lv_event_t *e)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *card;
  lv_obj_t *scan;
  (void)e;
  g_view = SETTINGS_VIEW_BLE;
  settings_new_overlay("蓝牙");
  card = lv_obj_create(g_overlay);
  ui_card(card);
  lv_obj_set_size(card, 292, 34);
  lv_obj_set_pos(card, 14, 34);
  g_detail = lv_label_create(g_overlay);
  lv_obj_set_width(g_detail, 192);
  lv_obj_set_pos(g_detail, 22, 42);
  lv_obj_set_style_text_font(g_detail, settings_font(), 0);
  lv_obj_set_style_text_color(g_detail, p->text, 0);
  scan = settings_button(card, "扫描", 54, 26);
  lv_obj_set_pos(scan, 226, 4);
  lv_obj_add_event_cb(scan, ble_scan_cb, LV_EVENT_CLICKED, NULL);
  g_list = lv_obj_create(g_overlay);
  ui_panel(g_list, p->bg);
  lv_obj_set_size(g_list, 292, 106);
  lv_obj_set_pos(g_list, 14, 74);
  lv_obj_set_style_pad_all(g_list, 2, 0);
  lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g_list, 3, 0);
  lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
  refresh_ble();
  settings_action_config(1, "扫描", ble_scan_cb);
  settings_action_config(2, "返回", settings_close_cb);
}

static void portal_open_cb(lv_event_t *e)
{
  (void)e;
  /* device_wifi_portal_open renews an active session in place. */
  device_wifi_portal_open(SETTINGS_PORTAL_TIMEOUT_SEC);
  settings_mark_dirty();
}

static void portal_close_cb(lv_event_t *e)
{
  (void)e;
  settings_close_cb(NULL);
}

static void refresh_portal(void)
{
  const ui_palette_t *p = settings_palette();
  device_wifi_portal_status_t portal;
  device_wifi_status_t wifi;
  char text[208];

  if (!g_detail)
    return;
  memset(&portal, 0, sizeof(portal));
  if (device_wifi_portal_get_status(&portal) < 0)
    {
      lv_label_set_text(g_detail, "无法读取热点状态。\n请点“续期 5m”重试。");
      if (g_qrcode) lv_obj_add_flag(g_qrcode, LV_OBJ_FLAG_HIDDEN);
      return;
    }
  if (portal.starting)
    {
      lv_label_set_text(g_detail, "正在启动临时热点…\n\n请稍候，二维码将在热点可用后显示。");
      if (g_qrcode) lv_obj_add_flag(g_qrcode, LV_OBJ_FLAG_HIDDEN);
      return;
    }
  if (!portal.active)
    {
      memset(&wifi, 0, sizeof(wifi));
      if (device_wifi_get_status(&wifi) == 0 &&
          wifi.state == DEVICE_WIFI_ONLINE)
        snprintf(text, sizeof(text),
                 "配网成功\n\n已连接：%s\n地址：%s\n\n请让手机或电脑接入同一 Wi-Fi，访问：\nhttp://%s/",
                 wifi.ssid[0] ? wifi.ssid : "Wi-Fi",
                 wifi.ip[0] ? wifi.ip : "--",
                 wifi.ip[0] ? wifi.ip : "设备地址");
      else if (portal.error != 0)
        snprintf(text, sizeof(text),
                 "无法开启临时热点（%d）。\n\n请点“续期 5m”重试。",
                 portal.error);
      else
        snprintf(text, sizeof(text),
                 "临时热点已关闭。\n\n如需重新配网，请点“续期 5m”。");
      lv_label_set_text(g_detail, text);
      if (g_qrcode) lv_obj_add_flag(g_qrcode, LV_OBJ_FLAG_HIDDEN);
      return;
    }

  if (g_qrcode)
    {
      char payload[128];
      snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
               portal.ssid, portal.password);
      if (strcmp(payload, g_portal_qr_payload) != 0)
        {
          if (lv_qrcode_update(g_qrcode, payload, strlen(payload)) == LV_RESULT_OK)
            snprintf(g_portal_qr_payload, sizeof(g_portal_qr_payload), "%s", payload);
        }
      lv_qrcode_set_dark_color(g_qrcode, p->text);
      lv_qrcode_set_light_color(g_qrcode, lv_color_hex(0xffffff));
      lv_obj_clear_flag(g_qrcode, LV_OBJ_FLAG_HIDDEN);
    }
  snprintf(text, sizeof(text),
           "1. 扫码加入热点\n网络：%s\n密码：%s\n\n2. 打开：\n%s\n剩余：%lu 秒",
           portal.ssid, portal.password, portal.url,
           (unsigned long)portal.remaining_seconds);
  lv_label_set_text(g_detail, text);
}

static void show_portal(void)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *card;
  g_view = SETTINGS_VIEW_PORTAL;
  g_portal_qr_payload[0] = '\0';
  settings_new_overlay("扫码配网");
  card = lv_obj_create(g_overlay);
  ui_card(card);
  lv_obj_set_size(card, 292, 142);
  lv_obj_set_pos(card, 14, 36);
  g_qrcode = lv_qrcode_create(card);
  lv_qrcode_set_size(g_qrcode, 116);
  lv_obj_set_pos(g_qrcode, 8, 13);
  g_detail = lv_label_create(card);
  lv_obj_set_width(g_detail, 154);
  lv_obj_set_pos(g_detail, 130, 9);
  lv_obj_set_style_text_font(g_detail, settings_font(), 0);
  lv_obj_set_style_text_color(g_detail, p->text, 0);
  refresh_portal();
  settings_action_config(0, "续期 5m", portal_open_cb);
  settings_action_config(2, "关闭", portal_close_cb);
  lv_obj_set_style_text_color(g_action_labels[2], p->err, 0);
}

static void show_portal_cb(lv_event_t *e)
{
  device_wifi_portal_status_t portal;
  (void)e;
  memset(&portal, 0, sizeof(portal));
  device_wifi_portal_get_status(&portal);
  if (!portal.active && !portal.starting)
    portal_open_cb(NULL);
  show_portal();
}

static void settings_auto_portal_check(void)
{
  device_wifi_status_t wifi;

  if (g_auto_portal_state != SETTINGS_AUTO_PORTAL_WAITING)
    return;
  if (device_wifi_get_status(&wifi) == 0 &&
      wifi.state == DEVICE_WIFI_ONLINE)
    {
      g_auto_portal_state = SETTINGS_AUTO_PORTAL_CANCELLED;
      return;
    }
  if ((int32_t)(lv_tick_get() - g_auto_portal_deadline) < 0)
    return;

  /* The factory connection may still be retrying after 30 seconds.
   * Starting a setup AP now would interrupt its use of the Wi-Fi radio. */
  if (device_wifi_bootstrap_in_progress())
    return;

  /* Do not replace a page the user opened themselves.  This boot's one
   * automatic prompt has still been consumed. */
  g_auto_portal_state = SETTINGS_AUTO_PORTAL_FIRED;
  if (g_overlay)
    return;
  portal_open_cb(NULL);
  show_portal();
}

static void show_info_cb(lv_event_t *e)
{
  const ui_palette_t *p = settings_palette();
  device_wifi_status_t wifi;
  char text[256];
  lv_obj_t *card;
  (void)e;
  g_view = SETTINGS_VIEW_INFO;
  settings_new_overlay("信息");
  memset(&wifi, 0, sizeof(wifi));
  device_wifi_get_status(&wifi);
  snprintf(text, sizeof(text),
           "板卡：Gemini-S1 / R528\n屏幕：%dx%d\n固件：openvela ai_agent\n"
           "Wi-Fi：%s\nIP：%s\n网关：%s\n配置：/data/ai_agent/config",
           g_width, g_height, wifi_state_text(wifi.state),
           wifi.ip[0] ? wifi.ip : "0.0.0.0",
           wifi.gateway[0] ? wifi.gateway : "0.0.0.0");
  card = lv_obj_create(g_overlay);
  ui_card(card);
  lv_obj_set_size(card, 292, 144);
  lv_obj_set_pos(card, 14, 36);
  g_detail = lv_label_create(g_overlay);
  lv_label_set_text(g_detail, text);
  lv_obj_set_width(g_detail, 272);
  lv_obj_set_style_text_font(g_detail, settings_font(), 0);
  lv_obj_set_style_text_color(g_detail, p->text, 0);
  lv_obj_set_pos(g_detail, 24, 44);
  settings_action_config(2, "返回", settings_close_cb);
}

static void show_home(void)
{
  const ui_palette_t *p = settings_palette();
  lv_obj_t *status_card;
  lv_obj_t *status_label;
  lv_obj_t *grid;
  lv_obj_t *button;
  g_view = SETTINGS_VIEW_HOME;
  settings_new_overlay("设备");
  status_card = settings_button(g_overlay, "Wi-Fi · Lab-AP    192.168.1.42",
                                292, 30);
  lv_obj_set_pos(status_card, 14, 34);
  status_label = lv_obj_get_child(status_card, 0);
  lv_obj_set_width(status_label, 274);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 8, 0);
  g_detail = status_label;
  grid = lv_obj_create(g_overlay);
  ui_panel(grid, p->bg);
  lv_obj_set_size(grid, 292, 112);
  lv_obj_set_pos(grid, 14, 68);
  button = settings_button(grid, "Wi-Fi\n已连接", 142, 50);
  lv_obj_set_pos(button, 0, 0);
  lv_obj_add_event_cb(button, show_wifi_cb, LV_EVENT_CLICKED, NULL);
  button = settings_button(grid, "蓝牙\n3 已配对", 142, 50);
  lv_obj_set_pos(button, 150, 0);
  lv_obj_add_event_cb(button, show_ble_cb, LV_EVENT_CLICKED, NULL);
  button = settings_button(grid, "扫码配网\n热点 · 网页", 142, 50);
  lv_obj_set_pos(button, 0, 58);
  lv_obj_add_event_cb(button, show_portal_cb, LV_EVENT_CLICKED, NULL);
  button = settings_button(grid, "设备信息\nR528", 142, 50);
  lv_obj_set_pos(button, 150, 58);
  lv_obj_add_event_cb(button, show_info_cb, LV_EVENT_CLICKED, NULL);
  settings_action_config(2, "关闭", settings_close_cb);
}

static void settings_timer_cb(lv_timer_t *timer)
{
  device_wifi_status_t wifi;
  char summary[96];
  (void)timer;

  settings_auto_portal_check();

  if (g_overlay && g_view == SETTINGS_VIEW_HOME && g_detail &&
      device_wifi_get_status(&wifi) == 0)
    {
      snprintf(summary, sizeof(summary), "Wi-Fi · %s    %s",
               wifi.ssid[0] ? wifi.ssid : "--",
               wifi.ip[0] ? wifi.ip : "0.0.0.0");
      lv_label_set_text(g_detail, summary);
    }

  if (!g_overlay)
    {
      return;
    }

  if (g_view == SETTINGS_VIEW_PORTAL)
    {
      refresh_portal();
    }

  if (!settings_take_dirty())
    {
      return;
    }
  if (g_view == SETTINGS_VIEW_WIFI)
    {
      refresh_wifi();
    }
  else if (g_view == SETTINGS_VIEW_BLE)
    {
      refresh_ble();
    }
}

int launcher_settings_ui_init(lv_obj_t *screen, const lv_font_t *font)
{
  lv_display_t *display;
  int ret;

  if (!screen || g_screen)
    {
      return -1;
    }

  g_screen = screen;
  g_font = font;
  g_auto_portal_state = SETTINGS_AUTO_PORTAL_WAITING;
  g_auto_portal_deadline = lv_tick_get() +
                          SETTINGS_AUTO_PORTAL_DELAY_MS;
  g_portal_qr_payload[0] = '\0';
  g_width = 320;
  g_height = 240;
  display = lv_display_get_default();
  if (display)
    {
      g_width = lv_display_get_horizontal_resolution(display);
      g_height = lv_display_get_vertical_resolution(display);
    }

  ret = device_settings_register_listener(settings_service_event, NULL);
  if (ret < 0)
    {
      g_screen = NULL;
      g_font = NULL;
      return ret;
    }
  g_timer = lv_timer_create(settings_timer_cb, 1000, NULL);
  if (!g_timer)
    {
      device_settings_unregister_listener(settings_service_event, NULL);
      g_screen = NULL;
      g_font = NULL;
      return -1;
    }

  settings_mark_dirty();
  return 0;
}

void launcher_settings_ui_deinit(void)
{
  device_wifi_portal_close();
  device_settings_unregister_listener(settings_service_event, NULL);
  if (g_timer)
    {
      lv_timer_delete(g_timer);
      g_timer = NULL;
    }

  if (g_overlay)
    {
      lv_obj_delete(g_overlay);
    }

  g_overlay = NULL;
  g_detail = NULL;
  g_list = NULL;
  g_screen = NULL;
  g_font = NULL;
  g_auto_portal_state = SETTINGS_AUTO_PORTAL_CANCELLED;
  g_auto_portal_deadline = 0;
  g_portal_qr_payload[0] = '\0';
  pthread_mutex_lock(&g_dirty_lock);
  g_dirty = false;
  pthread_mutex_unlock(&g_dirty_lock);
}

void launcher_settings_ui_open(void)
{
  if (g_screen)
    {
      if (g_auto_portal_state == SETTINGS_AUTO_PORTAL_WAITING)
        {
          g_auto_portal_state = SETTINGS_AUTO_PORTAL_CANCELLED;
        }
      show_home();
      settings_mark_dirty();
    }
}

bool launcher_settings_ui_is_open(void)
{
  return g_overlay != NULL;
}
