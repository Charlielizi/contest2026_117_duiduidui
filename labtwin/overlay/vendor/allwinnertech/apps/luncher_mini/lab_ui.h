#ifndef LAB_UI_H
#define LAB_UI_H

#include <lvgl/lvgl.h>
#include "lab_ui_types.h"
#include "ui_tokens.h"

/* 字号角色 - 复用 ui_tokens.h 的 ui_fonts_t (7 档) */
typedef ui_fonts_t lab_ui_fonts_t;

typedef struct lab_ui_s lab_ui_t;
typedef void (*lab_ui_command_cb_t)(const lab_ui_action_t *action, void *arg);

lab_ui_t *lab_ui_create(lv_obj_t *parent, const lab_ui_fonts_t *fonts,
                        lab_ui_command_cb_t command_cb, void *arg);
void lab_ui_destroy(lab_ui_t *ui);
void lab_ui_set_snapshot(lab_ui_t *ui, const lab_ui_snapshot_t *snapshot);

#endif /* LAB_UI_H */
