/**
 * @file app_store.c
 * @brief 商店应用（占位）
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_store_callbacks。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"

static const char *TAG = "APP_STORE";

static lv_obj_t *s_store_obj = NULL;
static int s_store_sel = 0;

static void store_init(void *data)
{
    ESP_LOGI(TAG, "Store init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "应用商店");
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_store_obj = list;
    ui_dock_create(scr, 1, 0);
}

static void store_destroy(void)
{
    ESP_LOGI(TAG, "Store destroy");
    s_store_obj = NULL;
}

static bool store_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_store_callbacks = {
    .init = store_init,
    .destroy = store_destroy,
    .on_key = store_on_key,
};