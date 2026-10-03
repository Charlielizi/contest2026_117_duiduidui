/****************************************************************************
 * Gemini-S1 launcher settings UI
 ****************************************************************************/

#ifndef __LUNCHER_MINI_DEVICE_SETTINGS_UI_H
#define __LUNCHER_MINI_DEVICE_SETTINGS_UI_H

#include <lvgl/lvgl.h>

int launcher_settings_ui_init(lv_obj_t *screen, const lv_font_t *font);
void launcher_settings_ui_deinit(void);
void launcher_settings_ui_open(void);
bool launcher_settings_ui_is_open(void);

#endif /* __LUNCHER_MINI_DEVICE_SETTINGS_UI_H */
