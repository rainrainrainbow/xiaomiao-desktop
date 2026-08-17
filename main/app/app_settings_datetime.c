/**
 * @file app_settings_datetime.c
 * @brief 日期时间设置二级页面 - 显示当前时间，预留NTP同步接口
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_datetime_settings_callbacks。
 * 显示当前日期时间，提供NTP同步功能（预留）。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "APP_DATETIME";

/* ========== UI状态 ========== */
static lv_obj_t *s_dt_list = NULL;
static lv_obj_t *s_dt_labels[4] = {0};
static int s_dt_sel = 0;
static int s_dt_vis_rows = 6;
static int s_dt_row_h = 14;
static int s_dt_total = 4;

static void dt_refresh_label(int idx)
{
    if (!s_dt_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];

    time_t now;
    struct tm *tm_info;
    time(&now);
    tm_info = localtime(&now);

    switch (idx) {
    case 0:
        snprintf(buf, sizeof(buf), "日期: %04d-%02d-%02d",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday);
        break;
    case 1:
        snprintf(buf, sizeof(buf), "时间: %02d:%02d:%02d",
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
        break;
    case 2:
        snprintf(buf, sizeof(buf), "NTP同步: 未实现");
        break;
    case 3:
        snprintf(buf, sizeof(buf), "B键返回");
        break;
    default:
        buf[0] = '\0';
        break;
    }
    lv_label_set_text(s_dt_labels[idx], buf);
}

static void dt_rebuild_visible(void)
{
    if (!s_dt_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_dt_list);
    memset(s_dt_labels, 0, sizeof(s_dt_labels));
    for (int i = 0; i < s_dt_vis_rows && i < s_dt_total; i++) {
        lv_obj_t *row = lv_obj_create(s_dt_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_dt_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_dt_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_dt_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_dt_labels[i] = lbl;
        dt_refresh_label(i);
    }
}

static void dt_settings_init(void *data)
{
    ESP_LOGI(TAG, "Datetime settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("日期时间");
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_dt_row_h = font_px + 2;
    s_dt_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_dt_row_h;
    if (s_dt_vis_rows < 1) s_dt_vis_rows = 1;
    s_dt_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dt_list);
    lv_obj_set_pos(s_dt_list, 0, ui_content_y());
    lv_obj_set_size(s_dt_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_dt_list, LV_OBJ_FLAG_SCROLLABLE);
    s_dt_sel = 0;
    dt_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void dt_settings_destroy(void)
{
    ESP_LOGI(TAG, "Datetime settings destroy");
    s_dt_list = NULL;
    memset(s_dt_labels, 0, sizeof(s_dt_labels));
}

static bool dt_settings_on_key(int key)
{
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP || key == KEY_DOWN) {
        /* 刷新显示时间 */
        dt_rebuild_visible();
        return true;
    }
    if (key == KEY_A) {
        /* 刷新时间 */
        dt_rebuild_visible();
        return true;
    }
    return false;
}

const page_callbacks_t g_datetime_settings_callbacks = {
    .init = dt_settings_init,
    .destroy = dt_settings_destroy,
    .on_key = dt_settings_on_key,
};