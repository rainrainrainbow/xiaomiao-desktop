/**
 * @file app_settings_theme.c
 * @brief 主题设置二级页面 - LVGL lv_checkbox 复选框选择
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_THEME";

#define THEME_OPTION_COUNT 2
static const char *s_theme_names[THEME_OPTION_COUNT] = {"深色主题", "浅色主题"};

static lv_obj_t *s_theme_list = NULL;
static lv_obj_t *s_theme_labels[THEME_OPTION_COUNT + 1] = {0};
static lv_obj_t *s_theme_checkboxes[THEME_OPTION_COUNT] = {0}; /* 复选框组件 */
static int s_theme_sel = 0;
static int s_theme_scroll = 0;
static int s_theme_vis_rows = 6;
static int s_theme_row_h = 14;
static int s_theme_total = THEME_OPTION_COUNT + 1;

static void theme_refresh_label(int idx)
{
    if (!s_theme_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    if (idx < THEME_OPTION_COUNT) {
        snprintf(buf, sizeof(buf), "%s", s_theme_names[idx]);
    } else {
        snprintf(buf, sizeof(buf), "当前: %s", s_theme_names[st->theme]);
    }
    lv_label_set_text(s_theme_labels[idx], buf);
}

/* 刷新复选框状态 */
static void theme_refresh_checkboxes(void)
{
    ui_state_t *st = ui_state_get();
    for (int i = 0; i < THEME_OPTION_COUNT; i++) {
        if (s_theme_checkboxes[i]) {
            if (i == (int)st->theme) {
                lv_obj_add_state(s_theme_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_theme_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void theme_rebuild_visible(void)
{
    if (!s_theme_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_theme_list);
    memset(s_theme_labels, 0, sizeof(s_theme_labels));
    memset(s_theme_checkboxes, 0, sizeof(s_theme_checkboxes));
    s_theme_total = THEME_OPTION_COUNT + 1;
    if (s_theme_sel < s_theme_scroll) s_theme_scroll = s_theme_sel;
    if (s_theme_sel >= s_theme_scroll + s_theme_vis_rows) s_theme_scroll = s_theme_sel - s_theme_vis_rows + 1;
    if (s_theme_scroll > s_theme_total - s_theme_vis_rows) s_theme_scroll = s_theme_total - s_theme_vis_rows;
    if (s_theme_scroll < 0) s_theme_scroll = 0;
    for (int i = 0; i < s_theme_vis_rows && (s_theme_scroll + i) < s_theme_total; i++) {
        int idx = s_theme_scroll + i;
        lv_obj_t *row = lv_obj_create(s_theme_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_theme_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_theme_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_theme_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        if (idx < THEME_OPTION_COUNT) {
            /* 使用 LVGL 复选框组件 */
            lv_obj_t *cb = lv_checkbox_create(row);
            lv_checkbox_set_text(cb, s_theme_names[idx]);
            lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
            /* 设置选中状态 */
            if (idx == (int)st->theme) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
            }
            s_theme_checkboxes[idx] = cb;
            s_theme_labels[idx] = lv_label_create(row); /* 占位，不使用 */
        } else {
            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
            s_theme_labels[idx] = lbl;
            theme_refresh_label(idx);
        }
    }
}

static void theme_settings_init(void *data)
{
    ESP_LOGI(TAG, "Theme settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("主题设置");
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_theme_row_h = font_px + 2;
    s_theme_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_theme_row_h;
    if (s_theme_vis_rows < 1) s_theme_vis_rows = 1;
    s_theme_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_theme_list);
    lv_obj_set_pos(s_theme_list, 0, ui_content_y());
    lv_obj_set_size(s_theme_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_theme_list, LV_OBJ_FLAG_SCROLLABLE);
    s_theme_sel = st->theme;
    s_theme_scroll = 0;
    theme_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void theme_settings_destroy(void)
{
    ESP_LOGI(TAG, "Theme settings destroy");
    s_theme_list = NULL;
    memset(s_theme_labels, 0, sizeof(s_theme_labels));
    memset(s_theme_checkboxes, 0, sizeof(s_theme_checkboxes));
}

static bool theme_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP) { s_theme_sel = (s_theme_sel - 1 + s_theme_total) % s_theme_total; theme_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_theme_sel = (s_theme_sel + 1) % s_theme_total; theme_rebuild_visible(); return true; }
    if (key == KEY_A) {
        if (s_theme_sel < THEME_OPTION_COUNT) {
            theme_type_t new_theme = (theme_type_t)s_theme_sel;
            if (new_theme != st->theme) { st->theme = new_theme; ui_theme_set(new_theme); theme_refresh_checkboxes(); theme_refresh_label(THEME_OPTION_COUNT); }
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_theme_settings_callbacks = {
    .init = theme_settings_init,
    .destroy = theme_settings_destroy,
    .on_key = theme_settings_on_key,
};