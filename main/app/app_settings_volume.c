/**
 * @file app_settings_volume.c
 * @brief 音量设置二级页面 - 百分比滑块调节
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_volume_settings_callbacks。
 * 提供0%-100%的音量调节，左右键步进10%，A键确认返回。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_VOLUME";

static lv_obj_t *s_vol_list = NULL;
static lv_obj_t *s_vol_labels[3] = {0};
static int s_vol_sel = 0;
static int s_vol_vis_rows = 6;
static int s_vol_row_h = 14;

static void vol_refresh_label(int idx)
{
    if (!s_vol_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "音量: %d%%", st->volume); break;
    case 1: {
        int bars = st->volume / 10;
        char bar[16]; memset(bar, 0, sizeof(bar));
        for (int i = 0; i < bars && i < 10; i++) bar[i] = '=';
        if (bars < 10) bar[bars] = '>';
        snprintf(buf, sizeof(buf), "[%s]", bar);
        break;
    }
    case 2: snprintf(buf, sizeof(buf), "← → 调节  A确认"); break;
    default: buf[0] = '\0'; break;
    }
    lv_label_set_text(s_vol_labels[idx], buf);
}

static void vol_rebuild_visible(void)
{
    if (!s_vol_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_vol_list);
    memset(s_vol_labels, 0, sizeof(s_vol_labels));
    for (int i = 0; i < s_vol_vis_rows && i < 3; i++) {
        lv_obj_t *row = lv_obj_create(s_vol_list);
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
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_vol_labels[i] = lbl;
        vol_refresh_label(i);
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
    ui_statusbar_set_title("音量设置");
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_vol_row_h = font_px + 2;
    s_vol_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_vol_row_h;
    if (s_vol_vis_rows < 1) s_vol_vis_rows = 1;
    s_vol_list = lv_obj_create(scr);
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
}

static bool vol_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_LEFT) {
        st->volume -= 10; if (st->volume < 0) st->volume = 0;
        vol_rebuild_visible(); return true;
    }
    if (key == KEY_RIGHT) {
        st->volume += 10; if (st->volume > 100) st->volume = 100;
        vol_rebuild_visible(); return true;
    }
    if (key == KEY_A) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    return false;
}

const page_callbacks_t g_volume_settings_callbacks = {
    .init = vol_settings_init,
    .destroy = vol_settings_destroy,
    .on_key = vol_settings_on_key,
};