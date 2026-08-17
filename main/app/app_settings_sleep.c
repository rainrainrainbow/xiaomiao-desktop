/**
 * @file app_settings_sleep.c
 * @brief 屏幕超时设置二级页面 - 休眠时间选择
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_SLEEP";

#define SLEEP_OPTION_COUNT 5
static const int s_sleep_values[SLEEP_OPTION_COUNT] = {0, 30, 60, 120, 300};
static const char *s_sleep_names[SLEEP_OPTION_COUNT] = {"永不", "30秒", "60秒", "2分钟", "5分钟"};

static lv_obj_t *s_sleep_list = NULL;
static lv_obj_t *s_sleep_labels[SLEEP_OPTION_COUNT + 1] = {0};
static int s_sleep_sel = 0;
static int s_sleep_scroll = 0;
static int s_sleep_vis_rows = 6;
static int s_sleep_row_h = 14;
static int s_sleep_total = SLEEP_OPTION_COUNT + 1;

static void sleep_refresh_label(int idx)
{
    if (!s_sleep_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    if (idx < SLEEP_OPTION_COUNT) {
        snprintf(buf, sizeof(buf), "%s %s", (s_sleep_values[idx] == st->sleep_timeout) ? "✓" : " ", s_sleep_names[idx]);
    } else {
        const char *cur = "永不";
        for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
            if (s_sleep_values[i] == st->sleep_timeout) { cur = s_sleep_names[i]; break; }
        }
        snprintf(buf, sizeof(buf), "当前: %s", cur);
    }
    lv_label_set_text(s_sleep_labels[idx], buf);
}

static void sleep_rebuild_visible(void)
{
    if (!s_sleep_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_sleep_list);
    memset(s_sleep_labels, 0, sizeof(s_sleep_labels));
    s_sleep_total = SLEEP_OPTION_COUNT + 1;
    if (s_sleep_sel < s_sleep_scroll) s_sleep_scroll = s_sleep_sel;
    if (s_sleep_sel >= s_sleep_scroll + s_sleep_vis_rows) s_sleep_scroll = s_sleep_sel - s_sleep_vis_rows + 1;
    if (s_sleep_scroll > s_sleep_total - s_sleep_vis_rows) s_sleep_scroll = s_sleep_total - s_sleep_vis_rows;
    if (s_sleep_scroll < 0) s_sleep_scroll = 0;
    for (int i = 0; i < s_sleep_vis_rows && (s_sleep_scroll + i) < s_sleep_total; i++) {
        int idx = s_sleep_scroll + i;
        lv_obj_t *row = lv_obj_create(s_sleep_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_sleep_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_sleep_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_sleep_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_sleep_labels[idx] = lbl;
        sleep_refresh_label(idx);
    }
}

static void sleep_settings_init(void *data)
{
    ESP_LOGI(TAG, "Sleep timeout settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("屏幕超时");
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_sleep_row_h = font_px + 2;
    s_sleep_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_sleep_row_h;
    if (s_sleep_vis_rows < 1) s_sleep_vis_rows = 1;
    s_sleep_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sleep_list);
    lv_obj_set_pos(s_sleep_list, 0, ui_content_y());
    lv_obj_set_size(s_sleep_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_sleep_list, LV_OBJ_FLAG_SCROLLABLE);
    s_sleep_sel = 0; s_sleep_scroll = 0;
    for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
        if (s_sleep_values[i] == st->sleep_timeout) { s_sleep_sel = i; break; }
    }
    sleep_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void sleep_settings_destroy(void)
{
    ESP_LOGI(TAG, "Sleep timeout settings destroy");
    s_sleep_list = NULL;
    memset(s_sleep_labels, 0, sizeof(s_sleep_labels));
}

static bool sleep_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP) { s_sleep_sel = (s_sleep_sel - 1 + s_sleep_total) % s_sleep_total; sleep_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_sleep_sel = (s_sleep_sel + 1) % s_sleep_total; sleep_rebuild_visible(); return true; }
    if (key == KEY_A) {
        if (s_sleep_sel < SLEEP_OPTION_COUNT) {
            int new_val = s_sleep_values[s_sleep_sel];
            if (new_val != st->sleep_timeout) { st->sleep_timeout = new_val; sleep_rebuild_visible(); }
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_sleep_settings_callbacks = {
    .init = sleep_settings_init,
    .destroy = sleep_settings_destroy,
    .on_key = sleep_settings_on_key,
};