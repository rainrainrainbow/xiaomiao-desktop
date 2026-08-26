/**
 * @file app_settings_volume.c
 * @brief 音量设置二级页面 - LVGL lv_slider 滑块交互调节
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_volume_settings_callbacks。
 * 提供0%-100%的音量调节，使用 lv_slider 滑块组件，左右键步进10%，A键确认返回。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_VOLUME";

/* ========== UI状态 ========== */
static lv_obj_t *s_vol_list = NULL;
static lv_obj_t *s_vol_labels[3] = {0};
static lv_obj_t *s_vol_slider = NULL; /* LVGL 滑块组件 */
static int s_vol_sel = 0;
static int s_vol_vis_rows = 6;
static int s_vol_row_h = 14;

static void vol_refresh_label(int idx)
{
    if (!s_vol_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_VOLUME), st->volume); break;
    case 1: snprintf(buf, sizeof(buf), "%s", lang_get(STR_VOLUME)); break;
    case 2: snprintf(buf, sizeof(buf), "%s", lang_get(STR_VOLUME_HINT)); break;
    default: buf[0] = '\0'; break;
    }
    lv_label_set_text(s_vol_labels[idx], buf);
}

/* 刷新滑块值 */
static void vol_refresh_slider(void)
{
    if (!s_vol_slider) return;
    ui_state_t *st = ui_state_get();
    lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
}

static void vol_rebuild_visible(void)
{
    if (!s_vol_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_vol_list);
    memset(s_vol_labels, 0, sizeof(s_vol_labels));
    s_vol_slider = NULL;
    for (int i = 0; i < s_vol_vis_rows && i < 3; i++) {
        lv_obj_t *row = lv_obj_create(s_vol_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_vol_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_vol_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_vol_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_vol_labels[i] = lbl;
        vol_refresh_label(i);

        /* 第二行：添加 LVGL 滑块组件（lv_slider，可交互） */
        if (i == 1) {
            lv_obj_t *slider = lv_slider_create(row);
            if (!slider) {
                ESP_LOGE(TAG, "lv_slider_create(slider) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            } else {
                lv_obj_remove_style_all(slider);
                /* 滑块轨道背景 */
                lv_obj_set_style_bg_color(slider, lv_color_hex(colors->border), 0);
                lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(slider, 3, 0);
                /* 滑块指示器（填充部分） */
                lv_obj_set_style_bg_color(slider, lv_color_hex(colors->text), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_radius(slider, 3, LV_PART_INDICATOR);
                /* 滑块旋钮（小圆点） */
                lv_obj_set_style_bg_color(slider, lv_color_hex(colors->text), LV_PART_KNOB);
                lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
                lv_obj_set_style_radius(slider, 4, LV_PART_KNOB);
                lv_obj_set_style_pad_all(slider, 2, LV_PART_KNOB);
                /* 位置：标签右侧 */
                lv_obj_set_size(slider, LCD_H_RES - 90, s_vol_row_h - 4);
                lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -6, 0);
                lv_slider_set_range(slider, 0, 100);
                lv_slider_set_value(slider, st->volume, LV_ANIM_OFF);
                s_vol_slider = slider;
            }
        }
    }
}

static void vol_settings_init(void *data)
{
    ESP_LOGI(TAG, "Volume settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_VOLUME));
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_vol_row_h = font_px + 2;
    s_vol_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_vol_row_h;
    if (s_vol_vis_rows < 1) s_vol_vis_rows = 1;
    s_vol_list = lv_obj_create(scr);
    if (!s_vol_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_vol_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_vol_list);
    lv_obj_set_pos(s_vol_list, 0, ui_content_y());
    lv_obj_set_size(s_vol_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_vol_list, LV_OBJ_FLAG_SCROLLABLE);
    s_vol_sel = 0;
    vol_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void vol_settings_destroy(void)
{
    ESP_LOGI(TAG, "Volume settings destroy");
    s_vol_list = NULL;
    memset(s_vol_labels, 0, sizeof(s_vol_labels));
    s_vol_slider = NULL;
}

static bool vol_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_LEFT) {
        st->volume -= 10; if (st->volume < 0) st->volume = 0;
        vol_refresh_slider(); vol_refresh_label(0); return true;
    }
    if (key == KEY_RIGHT) {
        st->volume += 10; if (st->volume > 100) st->volume = 100;
        vol_refresh_slider(); vol_refresh_label(0); return true;
    }
    if (key == KEY_A) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    return false;
}

const page_callbacks_t g_volume_settings_callbacks = {
    .init = vol_settings_init,
    .destroy = vol_settings_destroy,
    .on_key = vol_settings_on_key,
};