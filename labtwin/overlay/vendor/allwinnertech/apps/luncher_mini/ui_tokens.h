#ifndef UI_TOKENS_H
#define UI_TOKENS_H

#include <stdbool.h>
#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 主题 */
typedef enum
{
  UI_THEME_DARK = 0,
  UI_THEME_LIGHT
} ui_theme_t;

/* 字号角色 - 覆盖 v7 设计全部字号需求 */
typedef struct
{
  const lv_font_t *caption;  /* 最小字: 状态条 stat、键名 k   */
  const lv_font_t *small;    /* 次要正文                       */
  const lv_font_t *body;     /* 正文                           */
  const lv_font_t *label;    /* 标题、按钮文字                 */
  const lv_font_t *sub;      /* 副标题、小数字                 */
  const lv_font_t *display;  /* 中等数字(倒计时、PIN)          */
  const lv_font_t *mega;     /* 大数字(时钟)                   */
} ui_fonts_t;

/* 调色板 - 按主题切换 */
typedef struct
{
  lv_color_t bg;          /* 屏幕底          */
  lv_color_t surface;     /* 卡片/状态条     */
  lv_color_t surface_2;   /* 次卡片/键盘键   */
  lv_color_t surface_3;   /* 进度槽/占位     */
  lv_color_t border;      /* 强描边          */
  lv_color_t border_soft; /* 弱描边          */
  lv_color_t text;        /* 主文字          */
  lv_color_t text_2;      /* 次文字(近白)    */
  lv_color_t text_3;      /* 弱文字          */
  lv_color_t accent;      /* 主色 teal       */
  lv_color_t accent_2;    /* 主色亮档        */
  lv_color_t ok;          /* 成功            */
  lv_color_t warn;        /* 警告            */
  lv_color_t err;         /* 错误            */
  lv_color_t info;        /* 信息            */
} ui_palette_t;

/* 启动时调用一次，传入字体表 */
void ui_tokens_init(const ui_fonts_t *fonts);

const ui_fonts_t *ui_fonts(void);
const ui_palette_t *ui_pal(void);

void ui_set_theme(ui_theme_t theme);
ui_theme_t ui_get_theme(void);

/* 主题变化回调: 控件注册后, 主题切换时被调用 */
typedef void (*ui_theme_cb_t)(void *arg);
void ui_tokens_on_theme_change(ui_theme_cb_t cb, void *arg);

/* ===== helpers ===== */

/* 容器: 纯背景, 无边框, 不可滚动 */
void ui_panel(lv_obj_t *obj, lv_color_t bg);

/* 卡片: surface 底 + border_soft 边 + 圆角 8 + 不可滚动 */
void ui_card(lv_obj_t *obj);

/* 创建标签 */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);

/* 半透明主色背景(用于 pill / 胶囊高亮底), opa 0-255 */
lv_color_t ui_with_opa(lv_color_t color, uint8_t opa);

#ifdef __cplusplus
}
#endif

#endif /* UI_TOKENS_H */
