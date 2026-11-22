/**
 * @file app_settings_brightness.c
 * @brief 亮度设置二级页面 - LVGL lv_slider 滑块交互调节
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_brightness_settings_callbacks。
 * 提供10%-100%的亮度调节，使用 lv_slider 滑块组件，左右键步进10%，A键确认返回。
 */
#include "esp_heap_caps.h"
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "driver/drv_backlight.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_BRIGHTNESS";

/* ========== UI状态 ========== */
#define BR_ROW_AUTO 3   /* 自动亮度开关行（无光线传感器，仅展示且标注不支持） */
#define BR_TOTAL    4
static lv_obj_t *s_br_list = NULL;
static lv_obj_t *s_br_labels[BR_TOTAL] = {0};
static lv_obj_t *s_br_slider = NULL; /* LVGL 滑块组件 */
static lv_obj_t *s_br_auto_switch = NULL; /* 自动亮度开关（禁用态） */
static int s_br_sel = 0;
static int s_br_vis_rows = 6;
static int s_br_row_h = 14;

static void br_refresh_label(int idx)
{
    if (!s_br_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_BRIGHTNESS), st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s", lang_get(STR_BRIGHTNESS)); break;
    case 2: snprintf(buf, sizeof(buf), "%s", lang_get(STR_BRIGHTNESS_HINT)); break;
    /* 无环境光传感器：给出开关 UI 但明确标注不支持，不伪造自动调节 */
    case BR_ROW_AUTO: snprintf(buf, sizeof(buf), "%s: 不支持(需光线传感器)", lang_get(STR_SET_AUTO_BRIGHT)); break;
    default: buf[0] = '\0'; break;
    }
    lv_label_set_text(s_br_labels[idx], buf);
}

/* 刷新滑块值 */
static void br_refresh_slider(void)
{
    if (!s_br_slider) return;
    ui_state_t *st = ui_state_get();
    lv_slider_set_value(s_br_slider, st->brightness, LV_ANIM_OFF);
}

static void br_rebuild_visible(void)
{
    if (!s_br_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_br_list);
    memset(s_br_labels, 0, sizeof(s_br_labels));
    s_br_slider = NULL;
    s_br_auto_switch = NULL;
    for (int i = 0; i < s_br_vis_rows && i < BR_TOTAL; i++) {
        lv_obj_t *row = lv_obj_create(s_br_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
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
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_br_labels[i] = lbl;
        br_refresh_label(i);

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
                lv_obj_set_size(slider, LCD_H_RES - 90, s_br_row_h - 4);
                lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -6, 0);
                lv_slider_set_range(slider, 10, 100);
                lv_slider_set_value(slider, st->brightness, LV_ANIM_OFF);
                s_br_slider = slider;
            }
        }

        /* 第4行：自动亮度开关 UI —— 无光线传感器，置为禁用态并明确标注不支持 */
        if (i == BR_ROW_AUTO) {
            lv_obj_t *sw = lv_switch_create(row);
            if (!sw) {
                ESP_LOGE(TAG, "lv_switch_create(auto) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            } else {
                lv_obj_remove_style_all(sw);
                lv_obj_set_style_bg_color(sw, lv_color_hex(colors->border), 0);
                lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(sw, 8, 0);
                lv_obj_set_style_bg_color(sw, lv_color_hex(colors->text_dim), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_radius(sw, 8, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(sw, lv_color_hex(colors->bg), LV_PART_KNOB);
                lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
                lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
                lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
                lv_obj_set_size(sw, 32, s_br_row_h - 4);
                lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
                lv_obj_add_state(sw, LV_STATE_DISABLED);
                s_br_auto_switch = sw;
            }
        }
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
    ui_statusbar_set_title(lang_get(STR_BRIGHTNESS));
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_br_row_h = font_px + 2;
    s_br_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_br_row_h;
    if (s_br_vis_rows < 1) s_br_vis_rows = 1;
    s_br_list = lv_obj_create(scr);
    if (!s_br_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_br_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
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
    s_br_slider = NULL;
    s_br_auto_switch = NULL;
}

static bool br_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    /* 上下键切换焦点；默认焦点 0，原有左右键调节亮度行为不变 */
    if (key == KEY_UP)   { s_br_sel = (s_br_sel - 1 + BR_TOTAL) % BR_TOTAL; br_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_br_sel = (s_br_sel + 1) % BR_TOTAL; br_rebuild_visible(); return true; }
    /* 自动亮度行无传感器，左右键不生效（开关为禁用态） */
    if (s_br_sel == BR_ROW_AUTO) return true;
    if (key == KEY_LEFT) {
        st->brightness -= 10; if (st->brightness < 10) st->brightness = 10;
        drv_backlight_set_brightness(st->brightness); br_refresh_slider(); br_refresh_label(0); return true;
    }
    if (key == KEY_RIGHT) {
        st->brightness += 10; if (st->brightness > 100) st->brightness = 100;
        drv_backlight_set_brightness(st->brightness); br_refresh_slider(); br_refresh_label(0); return true;
    }
    if (key == KEY_A) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    return false;
}

const page_callbacks_t g_brightness_settings_callbacks = {
    .init = br_settings_init,
    .destroy = br_settings_destroy,
    .on_key = br_settings_on_key,
};