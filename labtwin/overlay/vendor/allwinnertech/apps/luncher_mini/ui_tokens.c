#include "ui_tokens.h"
#include <string.h>

#define UI_THEME_CB_MAX 4

static const ui_fonts_t *s_fonts;
static ui_theme_t s_theme = UI_THEME_DARK;
static bool s_inited;

static ui_palette_t s_dark;
static ui_palette_t s_light;

static ui_theme_cb_t s_cbs[UI_THEME_CB_MAX];
static void *s_cb_args[UI_THEME_CB_MAX];

static void init_palettes(void)
{
  /* DARK - 夜间: 深炭底 + 鲜青绿 + 近白文字 */
  s_dark.bg          = lv_color_hex(0x0B0F14);
  s_dark.surface     = lv_color_hex(0x161C26);
  s_dark.surface_2   = lv_color_hex(0x1E2632);
  s_dark.surface_3   = lv_color_hex(0x283341);
  s_dark.border      = lv_color_hex(0x2C3441);
  s_dark.border_soft = lv_color_hex(0x222B39);
  s_dark.text        = lv_color_hex(0xFFFFFF);
  s_dark.text_2      = lv_color_hex(0xD1D9E6);
  s_dark.text_3      = lv_color_hex(0x9BA8BD);
  s_dark.accent      = lv_color_hex(0x2DD4BF);
  s_dark.accent_2    = lv_color_hex(0x5EEAD4);
  s_dark.ok          = lv_color_hex(0x4ADE80);
  s_dark.warn        = lv_color_hex(0xFBBF24);
  s_dark.err         = lv_color_hex(0xF87171);
  s_dark.info        = lv_color_hex(0x7AB8FF);

  /* LIGHT - 白天: 暖白底 + 深 teal + 深墨文字 */
  s_light.bg          = lv_color_hex(0xF5F7FA);
  s_light.surface     = lv_color_hex(0xFFFFFF);
  s_light.surface_2   = lv_color_hex(0xF1F5F9);
  s_light.surface_3   = lv_color_hex(0xE2E8F0);
  s_light.border      = lv_color_hex(0xD8DEE7);
  s_light.border_soft = lv_color_hex(0xE5EAF1);
  s_light.text        = lv_color_hex(0x0F172A);
  s_light.text_2      = lv_color_hex(0x334155);
  s_light.text_3      = lv_color_hex(0x64748B);
  s_light.accent      = lv_color_hex(0x0D9488);
  s_light.accent_2    = lv_color_hex(0x14B8A6);
  s_light.ok          = lv_color_hex(0x16A34A);
  s_light.warn        = lv_color_hex(0xD97706);
  s_light.err         = lv_color_hex(0xDC2626);
  s_light.info        = lv_color_hex(0x2563EB);
}

void ui_tokens_init(const ui_fonts_t *fonts)
{
  if (s_inited)
    {
      return;
    }
  s_fonts = fonts;
  init_palettes();
  s_inited = true;
}

const ui_fonts_t *ui_fonts(void)
{
  return s_fonts;
}

const ui_palette_t *ui_pal(void)
{
  return s_theme == UI_THEME_DARK ? &s_dark : &s_light;
}

void ui_set_theme(ui_theme_t theme)
{
  int i;
  if (theme == s_theme)
    {
      return;
    }
  s_theme = theme;
  for (i = 0; i < UI_THEME_CB_MAX; i++)
    {
      if (s_cbs[i])
        {
          s_cbs[i](s_cb_args[i]);
        }
    }
}

ui_theme_t ui_get_theme(void)
{
  return s_theme;
}

void ui_tokens_on_theme_change(ui_theme_cb_t cb, void *arg)
{
  int i;
  for (i = 0; i < UI_THEME_CB_MAX; i++)
    {
      if (!s_cbs[i])
        {
          s_cbs[i] = cb;
          s_cb_args[i] = arg;
          return;
        }
    }
}

void ui_panel(lv_obj_t *obj, lv_color_t bg)
{
  lv_obj_set_style_bg_color(obj, bg, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 0, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_card(lv_obj_t *obj)
{
  const ui_palette_t *p = ui_pal();
  lv_obj_set_style_bg_color(obj, p->surface, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, p->border_soft, 0);
  lv_obj_set_style_radius(obj, 8, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *lbl = lv_label_create(parent);
  if (font)
    {
      lv_obj_set_style_text_font(lbl, font, 0);
    }
  lv_obj_set_style_text_color(lbl, color, 0);
  return lbl;
}

lv_color_t ui_with_opa(lv_color_t color, uint8_t opa)
{
  lv_color32_t c32;
  c32.red = color.red;
  c32.green = color.green;
  c32.blue = color.blue;
  c32.alpha = opa;
  return lv_color_make(c32.red, c32.green, c32.blue);
}
