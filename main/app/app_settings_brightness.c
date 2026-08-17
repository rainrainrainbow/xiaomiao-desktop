/**
 * @file app_settings_brightness.c
 * @brief 亮度设置二级页面 - 百分比滑块调节
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_brightness_settings_callbacks。
 * 提供10%-100%的亮度调节，左右键步进10%，A键确认返回。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "fonts/lv_freetype_font.h"
#include "driver/drv_backlight.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_BRIGHTNESS";

/* ========== UI状态 ========== */
static lv_obj_t *s_br_list = NULL;
static lv_obj_t *s_br_labels[3] = {0};
static int s_br_sel = 0;
static int s_br_vis_rows = 6;
static int s_br_row_h = 14;

static void br_refresh_label(int idx)
{
    if (!s_br_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "亮度: %d%%", st->brightness); break;
    case 1: {
        int bars = st->brightness / 10;
        char bar[16]; memset(bar, 0, sizeof(bar));
        for (int i = 0; i < bars && i < 10; i++) bar[i] = '=';
        if (bars < 10) bar[bars] = '>';
        snprintf(buf, sizeof(buf), "[%s]", bar);
        break;
    }
    case 2: snprintf(buf, sizeof(buf), "← → 调节  A确认"); break;
    default: buf[0] = '\0'; break;
    }
    lv_label_set_text(s_br_labels[idx], buf);
}

static void br_rebuild_visible(void)
{
    if (!s_br_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_br_list);
    memset(s_br_labels, 0, sizeof(s_br_labels));
    for (int i = 0; i < s_br_vis_rows && i < 3; i++) {
        lv_obj_t *row = lv_obj_create(s_br_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_br_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_br_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_br_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_br_labels[i] = lbl;
        br_refresh_label(i);
    }
}

static void br_settings_init(void *data)
{
    ESP_LOGI(TAG, "Brightness settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("亮度设置");
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_br_row_h = font_px + 2;
    s_br_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_br_row_h;
    if (s_br_vis_rows < 1) s_br_vis_rows = 1;
    s_br_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_br_list);
    lv_obj_set_pos(s_br_list, 0, ui_content_y());
    lv_obj_set_size(s_br_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_br_list, LV_OBJ_FLAG_SCROLLABLE);
    s_br_sel = 0;
    br_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void br_settings_destroy(void)
{
    ESP_LOGI(TAG, "Brightness settings destroy");
    s_br_list = NULL;
    memset(s_br_labels, 0, sizeof(s_br_labels));
}

static bool br_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_LEFT) {
        st->brightness -= 10; if (st->brightness < 10) st->brightness = 10;
        drv_backlight_set_brightness(st->brightness); br_rebuild_visible(); return true;
    }
    if (key == KEY_RIGHT) {
        st->brightness += 10; if (st->brightness > 100) st->brightness = 100;
        drv_backlight_set_brightness(st->brightness); br_rebuild_visible(); return true;
    }
    if (key == KEY_A) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    return false;
}

const page_callbacks_t g_brightness_settings_callbacks = {
    .init = br_settings_init,
    .destroy = br_settings_destroy,
    .on_key = br_settings_on_key,
};