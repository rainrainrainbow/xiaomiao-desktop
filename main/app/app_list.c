/**
 * @file app_list.c
 * @brief 应用管理二级页面
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_applist_callbacks。
 * 参考 LiClock 的 App 架构设计，每个 App 独立文件。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <string.h>

static const char *TAG = "APP_LIST";

static lv_obj_t *s_applist_obj = NULL;
static int s_applist_sel = 0;
static int s_applist_total = 0;

static void applist_init(void *data)
{
    ESP_LOGI(TAG, "App list init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "应用管理");

    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    int py_count = 0;
    const app_def_t *py_apps = app_manager_get_micropython(&py_count);
    s_applist_total = builtin_count + py_count;
    if (s_applist_total <= 0) s_applist_total = 1;

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    int item_h = (LCD_V_RES - 26 - DOCK_H) / s_applist_total;
    if (item_h < 12) item_h = 12;
    if (item_h > 18) item_h = 18;

    int row_idx = 0;
    for (int i = 0; i < builtin_count; i++, row_idx++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, builtin_apps[i].icon_text);
        lv_obj_set_style_text_color(icon, lv_color_hex(builtin_apps[i].icon_color), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, builtin_apps[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *type_lbl = lv_label_create(row);
        lv_label_set_text(type_lbl, "内置");
        lv_obj_set_style_text_color(type_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_align(type_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    for (int i = 0; i < py_count; i++, row_idx++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, py_apps[i].icon_text);
        lv_obj_set_style_text_color(icon, lv_color_hex(py_apps[i].icon_color), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, py_apps[i].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *status_lbl = lv_label_create(row);
        if (py_apps[i].install_status == APP_INSTALL_OK) {
            lv_label_set_text(status_lbl, "Python");
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x22C55E), 0);
        } else {
            lv_label_set_text(status_lbl, app_install_status_desc(py_apps[i].install_status));
            lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xEF4444), 0);
        }
        lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    if (builtin_count == 0 && py_count == 0) {
        lv_obj_t *lbl = lv_label_create(list);
        lv_label_set_text(lbl, "暂无应用");
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }
    s_applist_obj = list;
    ui_dock_create(scr, 1, 0);
}

static void applist_destroy(void)
{
    ESP_LOGI(TAG, "App list destroy");
    s_applist_obj = NULL;
    s_applist_sel = 0;
    s_applist_total = 0;
}

static bool applist_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    int py_count = 0;
    const app_def_t *py_apps = app_manager_get_micropython(&py_count);
    s_applist_total = builtin_count + py_count;
    if (s_applist_total <= 0) return true;
    lv_obj_t *list = s_applist_obj;
    if (!list) return false;
    lv_obj_t *old_row = lv_obj_get_child(list, s_applist_sel);
    if (old_row) {
        lv_obj_set_style_bg_opa(old_row, LV_OPA_TRANSP, 0);
        lv_obj_t *old_lbl = lv_obj_get_child(old_row, 1);
        if (old_lbl) lv_obj_set_style_text_color(old_lbl, lv_color_hex(ui_theme_colors()->text), 0);
    }
    if (key == KEY_UP) s_applist_sel = (s_applist_sel - 1 + s_applist_total) % s_applist_total;
    if (key == KEY_DOWN) s_applist_sel = (s_applist_sel + 1) % s_applist_total;
    lv_obj_t *new_row = lv_obj_get_child(list, s_applist_sel);
    if (new_row) {
        lv_obj_set_style_bg_color(new_row, lv_color_hex(0x5C4220), 0);
        lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, 0);
        lv_obj_t *new_lbl = lv_obj_get_child(new_row, 1);
        if (new_lbl) lv_obj_set_style_text_color(new_lbl, lv_color_hex(0xF6D34A), 0);
    }
    if (key == KEY_A) {
        if (s_applist_sel < builtin_count) {
            app_manager_launch(&builtin_apps[s_applist_sel]);
        } else {
            int py_idx = s_applist_sel - builtin_count;
            if (py_idx < py_count) {
                if (py_apps[py_idx].install_status == APP_INSTALL_OK) {
                    app_manager_launch(&py_apps[py_idx]);
                } else {
                    ESP_LOGW(TAG, "Cannot launch blocked app: %s", py_apps[py_idx].name);
                }
            }
        }
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_applist_callbacks = {
    .init = applist_init,
    .destroy = applist_destroy,
    .on_key = applist_on_key,
};