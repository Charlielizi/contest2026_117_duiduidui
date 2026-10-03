/* SPDX-License-Identifier: Apache-2.0
 * Documentation-only capture harness: unchanged board C UI, existing demo data,
 * offscreen SDL software renderer. No board service, network or OS input.
 * See docs/SIMULATOR_UI.md for dependencies and reproduction.
 */
#define SDL_MAIN_HANDLED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/sdl/lv_sdl_window.h>
#include <lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h>
#include <lvgl/src/libs/lodepng/lodepng.h>
#include "lab_ui.h"
#include "lab_ui_demo.h"

static unsigned char *read_font(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
        length > 32 * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

static int select_scene(const char *scene)
{
    static const char *const pages[] = {"idle", "running", "timers", "voice", "alert"};
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (!strcmp(scene, pages[i])) {
            lab_ui_demo_set_page((lab_ui_page_t)i);
            return 0;
        }
    }
    if (!strcmp(scene, "picker") || !strcmp(scene, "paused") ||
        !strcmp(scene, "end-confirm") || !strcmp(scene, "complete-confirm")) {
        lab_ui_demo_set_scenario(scene);
        return 0;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "Usage: capture_ui SCENE OUTPUT.png FONT.ttf [light]\n");
        return 2;
    }
    if (argc == 5 && strcmp(argv[4], "light")) return 2;
    size_t size = 0;
    unsigned char *font_data = read_font(argv[3], &size);
    if (!font_data) {
        fprintf(stderr, "Font unavailable\n");
        return 3;
    }
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    lv_init();
    lv_display_t *display = lv_sdl_window_create(320, 240);
    if (!display) return 4;
    const unsigned sizes[] = {10, 11, 12, 14, 16, 28, 40};
    lv_font_t *fonts[7];
    for (unsigned i = 0; i < 7; ++i) {
        fonts[i] = lv_tiny_ttf_create_data(font_data, size, sizes[i]);
        if (!fonts[i]) return 5;
        fonts[i]->fallback = &lv_font_simsun_16_cjk;
    }
    lab_ui_fonts_t roles = {
        .caption = fonts[0], .small = fonts[1], .body = fonts[2],
        .label = fonts[3], .sub = fonts[4], .display = fonts[5], .mega = fonts[6]
    };
    ui_tokens_init(&roles);
    if (argc == 5) ui_set_theme(UI_THEME_LIGHT);
    lab_ui_t *ui = lab_ui_create(lv_screen_active(), &roles,
                                  lab_ui_demo_handle_command, NULL);
    if (!ui || lab_ui_demo_start(ui, NULL, NULL) || select_scene(argv[1])) return 6;
    lv_obj_update_layout(lv_screen_active());
    lv_timer_handler();
    lv_refr_now(display);
    SDL_Renderer *renderer = lv_sdl_window_get_renderer(display);
    int width = 0, height = 0;
    if (!renderer || SDL_GetRendererOutputSize(renderer, &width, &height) ||
        width != 320 || height != 240) return 7;
    unsigned char *rgba = malloc((size_t)width * height * 4);
    if (!rgba || SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGBA32,
                                     rgba, width * 4)) return 8;
    unsigned char *png = NULL;
    size_t png_size = 0;
    unsigned error = lodepng_encode32(&png, &png_size, rgba, (unsigned)width, (unsigned)height);
    free(rgba);
    if (error) {
        lv_free(png);
        fprintf(stderr, "PNG write error: %u\n", error);
        return 9;
    }
    /* This desktop harness uses stdio, not a board LVGL filesystem drive. */
    FILE *output = fopen(argv[2], "wb");
    if (!output) { lv_free(png); return 10; }
    size_t written = fwrite(png, 1, png_size, output);
    int close_error = fclose(output);
    lv_free(png);
    if (written != png_size || close_error) return 10;
    printf("UI_CAPTURE=PASS scene=%s pixels=320x240 data=existing-demo-mock\n", argv[1]);
    lab_ui_demo_stop();
    lab_ui_destroy(ui);
    lv_sdl_quit();
    return 0;
}
