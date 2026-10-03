#ifndef LAB_UI_DEMO_H
#define LAB_UI_DEMO_H

#include "lab_ui.h"

typedef void (*lab_ui_demo_settings_cb_t)(void *arg);

int lab_ui_demo_start(lab_ui_t *ui, lab_ui_demo_settings_cb_t settings_cb,
                      void *arg);
void lab_ui_demo_stop(void);
void lab_ui_demo_set_page(lab_ui_page_t page);
void lab_ui_demo_set_scenario(const char *scenario);
void lab_ui_demo_randomize(void);
void lab_ui_demo_handle_command(const lab_ui_action_t *action, void *arg);

#endif
