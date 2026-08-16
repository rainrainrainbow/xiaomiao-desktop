/**
 * @file app_music.c
 * @brief 音乐应用（占位）
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_music_callbacks。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"

static const char *TAG = "APP_MUSIC";

static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music init - placeholder");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF6D34A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "音乐");
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "敬请期待");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x1B1713), 0);
    lv_obj_set_style_text_font(lbl, lv_font_cn_get(14), 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 6);
}

static void music_destroy(void) { ESP_LOGI(TAG, "Music destroy"); }

static bool music_on_key(int key) {
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};