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
#include "lang/lang.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "fonts/lv_freetype_font.h"
#include <string.h>

static const char *TAG = "APP_LIST";

static lv_obj_t *s_applist_obj = NULL;
static lv_obj_t *s_applist_labels[32] = {0};
static int s_applist_sel = 0;
static int s_applist_scroll = 0;
static int s_applist_total = 0;
static int s_applist_row_h = 16;

static void applist_rebuild_visible(void)
{
    if (!s_applist_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_applist_obj);
    memset(s_applist_labels, 0, sizeof(s_applist_labels));

    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    int py_count = 0;
    const app_def_t *py_apps = app_manager_get_micropython(&py_count);
    s_applist_total = builtin_count + py_count;
    if (s_applist_total <= 0) s_applist_total = 1;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_applist_row_h = font_px + 2;

    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_applist_row_h;
    if (vis_rows < 1) vis_rows = 1;

    /* 确保选中项可见 */
    if (s_applist_sel < s_applist_scroll) s_applist_scroll = s_applist_sel;
    if (s_applist_sel >= s_applist_scroll + vis_rows) s_applist_scroll = s_applist_sel - vis_rows + 1;
    if (s_applist_scroll > s_applist_total - vis_rows) s_applist_scroll = s_applist_total - vis_rows;
    if (s_applist_scroll < 0) s_applist_scroll = 0;

    int row_idx = 0;
    for (int i = 0; i < builtin_count && row_idx < vis_rows; i++) {
        int idx = s_applist_scroll + row_idx;
        if (idx >= builtin_count + py_count) break;
        lv_obj_t *row = lv_obj_create(s_applist_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(builtin_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_applist_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_applist_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_applist_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *icon = lv_label_create(row);
        if (icon) {
            lv_label_set_text(icon, builtin_apps[idx].icon_text);
            lv_obj_set_style_text_color(icon, lv_color_hex(builtin_apps[idx].icon_color), 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(builtin_lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_label_set_text(lbl, app_builtin_get_display_name(builtin_apps[idx].name));
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 60);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *type_lbl = lv_label_create(row);
        if (type_lbl) {
            lv_label_set_text(type_lbl, lang_get(STR_APP_TYPE_BUILTIN));
            lv_obj_set_style_text_color(type_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(type_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
        }
        s_applist_labels[row_idx] = lbl;
        row_idx++;
    }
    for (int i = 0; i < py_count && row_idx < vis_rows; i++) {
        int idx = s_applist_scroll + row_idx;
        if (idx >= builtin_count + py_count) break;
        int py_idx = idx - builtin_count;
        if (py_idx < 0 || py_idx >= py_count) { row_idx++; continue; }
        lv_obj_t *row = lv_obj_create(s_applist_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(py_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_applist_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_applist_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_applist_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *icon = lv_label_create(row);
        if (icon) {
            lv_label_set_text(icon, py_apps[py_idx].icon_text);
            lv_obj_set_style_text_color(icon, lv_color_hex(py_apps[py_idx].icon_color), 0);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(py_lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_label_set_text(lbl, py_apps[py_idx].name);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 60);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 22, 0);
        lv_obj_t *status_lbl = lv_label_create(row);
        if (status_lbl) {
            if (py_apps[py_idx].install_status == APP_INSTALL_OK) {
                lv_label_set_text(status_lbl, "Python");
                lv_obj_set_style_text_color(status_lbl, lv_color_hex(0x22C55E), 0);
            } else {
                lv_label_set_text(status_lbl, app_install_status_desc(py_apps[py_idx].install_status));
                lv_obj_set_style_text_color(status_lbl, lv_color_hex(0xEF4444), 0);
            }
            lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
        }
        s_applist_labels[row_idx] = lbl;
        row_idx++;
    }
    if (builtin_count == 0 && py_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_applist_obj);
        if (lbl) {
            lv_label_set_text(lbl, lang_get(STR_RECENTS_EMPTY));
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        }
    }
}
static void applist_init(void *data)
{
    ESP_LOGI(TAG, "App list init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_MANAGER));
    lv_obj_t *list = lv_obj_create(scr);
    if (!list) {
        ESP_LOGE(TAG, "lv_obj_create(applist) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_applist_obj = list;
    s_applist_sel = 0;
    s_applist_scroll = 0;
    applist_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}
static void applist_destroy(void)
{
    ESP_LOGI(TAG, "App list destroy");
    s_applist_obj = NULL;
    memset(s_applist_labels, 0, sizeof(s_applist_labels));
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
    app_manager_get_builtin(&builtin_count);
    int py_count = 0;
    app_manager_get_micropython(&py_count);
    s_applist_total = builtin_count + py_count;
    if (s_applist_total <= 0) return true;

    if (key == KEY_UP) {
        s_applist_sel = (s_applist_sel - 1 + s_applist_total) % s_applist_total;
        applist_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_applist_sel = (s_applist_sel + 1) % s_applist_total;
        applist_rebuild_visible();
        return true;
    }
    if (key == KEY_A) {
        const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
        const app_def_t *py_apps = app_manager_get_micropython(&py_count);
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