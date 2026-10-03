#pragma once

#include "lab_ui.h"

typedef void (*lab_ui_controller_settings_cb_t)(void *arg);
int lab_ui_controller_start(lab_ui_t *ui,
                            lab_ui_controller_settings_cb_t settings_cb,
                            void *arg);
void lab_ui_controller_stop(void);
void lab_ui_controller_handle_command(const lab_ui_action_t *action, void *arg);
