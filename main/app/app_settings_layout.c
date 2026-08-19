/**
 * @file app_settings_layout.c
 * @brief 布局设置二级页面 - 桌面图标布局选择
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_LAYOUT";

#define LAYOUT_OPTION_COUNT 2
/* 国际化：使用 lang_get 获取布局名和描述 */
static const char *layout_name(int idx) {
    return idx == 0 ? lang_get(STR_LAYOUT_3COL) : lang_get(STR_LAYOUT_2COL);
}
static const char *layout_desc(int idx) {
    return idx == 0 ? lang_get(STR_LAYOUT_3COL_DESC) : lang_get(STR_LAYOUT_2COL_DESC);
}

static lv_obj_t *s_layout_list = NULL;
static lv_obj_t *s_layout_labels[LAYOUT_OPTION_COUNT * 2 + 1] = {0};
static lv_obj_t *s_layout_checkboxes[LAYOUT_OPTION_COUNT] = {0}; /* 复选框组件 */
static int s_layout_sel = 0;
static int s_layout_scroll = 0;
static int s_layout_vis_rows = 6;
static int s_layout_row_h = 14;
static int s_layout_total = LAYOUT_OPTION_COUNT * 2 + 1;

/* 刷新复选框状态 */
static void layout_refresh_checkboxes(void)
{
    ui_state_t *st = ui_state_get();
    for (int i = 0; i < LAYOUT_OPTION_COUNT; i++) {
        if (s_layout_checkboxes[i]) {
            if (i == (int)st->layout) {
                lv_obj_add_state(s_layout_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_layout_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void layout_refresh_label(int idx)
{
    if (!s_layout_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    if (idx < LAYOUT_OPTION_COUNT) {
        snprintf(buf, sizeof(buf), "%s", layout_name(idx));
    } else if (idx < LAYOUT_OPTION_COUNT * 2) {
        snprintf(buf, sizeof(buf), "  %s", layout_desc(idx - LAYOUT_OPTION_COUNT));
    } else {
        snprintf(buf, sizeof(buf), "%s", lang_get(STR_LAYOUT_HINT));
    }
    lv_label_set_text(s_layout_labels[idx], buf);
}

static void layout_rebuild_visible(void)
{
    if (!s_layout_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_layout_list);
    memset(s_layout_labels, 0, sizeof(s_layout_labels));
    memset(s_layout_checkboxes, 0, sizeof(s_layout_checkboxes));
    s_layout_total = LAYOUT_OPTION_COUNT * 2 + 1;
    if (s_layout_sel < s_layout_scroll) s_layout_scroll = s_layout_sel;
    if (s_layout_sel >= s_layout_scroll + s_layout_vis_rows) s_layout_scroll = s_layout_sel - s_layout_vis_rows + 1;
    if (s_layout_scroll > s_layout_total - s_layout_vis_rows) s_layout_scroll = s_layout_total - s_layout_vis_rows;
    if (s_layout_scroll < 0) s_layout_scroll = 0;
    for (int i = 0; i < s_layout_vis_rows && (s_layout_scroll + i) < s_layout_total; i++) {
        int idx = s_layout_scroll + i;
        lv_obj_t *row = lv_obj_create(s_layout_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_layout_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_layout_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_layout_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        if (idx < LAYOUT_OPTION_COUNT) {
            /* 使用LVGL复选框组件 */
            lv_obj_t *cb = lv_checkbox_create(row);
            lv_checkbox_set_text(cb, layout_name(idx));
            lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
            if (idx == (int)st->layout) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
            }
            s_layout_checkboxes[idx] = cb;
            s_layout_labels[idx] = lv_label_create(row); /* 占位 */
        } else {
            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
            s_layout_labels[idx] = lbl;
            layout_refresh_label(idx);
        }
    }
}

static void layout_settings_init(void *data)
{
    ESP_LOGI(TAG, "Layout settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_LAYOUT));
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_layout_row_h = font_px + 2;
    s_layout_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_layout_row_h;
    if (s_layout_vis_rows < 1) s_layout_vis_rows = 1;
    s_layout_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_layout_list);
    lv_obj_set_pos(s_layout_list, 0, ui_content_y());
    lv_obj_set_size(s_layout_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_layout_list, LV_OBJ_FLAG_SCROLLABLE);
    s_layout_sel = 0; s_layout_scroll = 0;
    layout_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void layout_settings_destroy(void)
{
    ESP_LOGI(TAG, "Layout settings destroy");
    s_layout_list = NULL;
    memset(s_layout_labels, 0, sizeof(s_layout_labels));
    memset(s_layout_checkboxes, 0, sizeof(s_layout_checkboxes));
}

static bool layout_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP) { s_layout_sel = (s_layout_sel - 1 + s_layout_total) % s_layout_total; layout_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_layout_sel = (s_layout_sel + 1) % s_layout_total; layout_rebuild_visible(); return true; }
    if (key == KEY_A) {
        if (s_layout_sel < LAYOUT_OPTION_COUNT) {
            if (s_layout_sel != st->layout) { st->layout = s_layout_sel; layout_refresh_checkboxes(); layout_rebuild_visible(); }
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_layout_settings_callbacks = {
    .init = layout_settings_init,
    .destroy = layout_settings_destroy,
    .on_key = layout_settings_on_key,
};