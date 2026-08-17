/**
 * @file app_settings_font.c
 * @brief 字体设置二级页面 - 选择字体大小并预览
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_font_settings_callbacks。
 * 提供4种字体大小（14px/16px/20px/24px）的选择和实时预览。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_FONT";

/* ========== 字体选项 ========== */
#define FONT_OPTION_COUNT 4
static const int s_font_sizes[FONT_OPTION_COUNT] = {14, 16, 20, 24};
static const char *s_font_labels[FONT_OPTION_COUNT] = {"小 (14px)", "中 (16px)", "大 (20px)", "特大 (24px)"};

/* ========== UI状态 ========== */
static lv_obj_t *s_font_list = NULL;
static lv_obj_t *s_font_labels[FONT_OPTION_COUNT + 1] = {0};  /* +1: 预览行 */
static int s_font_sel = 0;
static int s_font_scroll = 0;
static int s_font_vis_rows = 6;
static int s_font_row_h = 14;
static int s_font_total = FONT_OPTION_COUNT + 1;  /* 选项 + 预览 */
static lv_obj_t *s_preview_label = NULL;

static void font_refresh_label(int idx)
{
    if (!s_font_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];

    if (idx < FONT_OPTION_COUNT) {
        /* 字体选项行 */
        int size = s_font_sizes[idx];
        if (size == st->font_size) {
            snprintf(buf, sizeof(buf), "✓ %s", s_font_labels[idx]);
        } else {
            snprintf(buf, sizeof(buf), "  %s", s_font_labels[idx]);
        }
    } else {
        /* 预览行 */
        snprintf(buf, sizeof(buf), "预览: 小喵桌面 %dpx", st->font_size);
    }
    lv_label_set_text(s_font_labels[idx], buf);
}

static void font_rebuild_visible(void)
{
    if (!s_font_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_font_list);
    memset(s_font_labels, 0, sizeof(s_font_labels));

    s_font_total = FONT_OPTION_COUNT + 1;

    /* 确保选中项在可见范围内 */
    if (s_font_sel < s_font_scroll) s_font_scroll = s_font_sel;
    if (s_font_sel >= s_font_scroll + s_font_vis_rows) {
        s_font_scroll = s_font_sel - s_font_vis_rows + 1;
    }
    if (s_font_scroll > s_font_total - s_font_vis_rows) {
        s_font_scroll = s_font_total - s_font_vis_rows;
    }
    if (s_font_scroll < 0) s_font_scroll = 0;

    for (int i = 0; i < s_font_vis_rows && (s_font_scroll + i) < s_font_total; i++) {
        int idx = s_font_scroll + i;
        lv_obj_t *row = lv_obj_create(s_font_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_font_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_font_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (idx == s_font_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);

        if (idx < FONT_OPTION_COUNT) {
            /* 字体选项使用当前字体大小显示 */
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        } else {
            /* 预览行使用选中的字体大小显示 */
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        }

        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_font_labels[idx] = lbl;
        font_refresh_label(idx);
    }
}

/* ========== 页面生命周期 ========== */
static void font_settings_init(void *data)
{
    ESP_LOGI(TAG, "Font settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("字体设置");

    /* 计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_font_row_h = font_px + 2;
    s_font_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_font_row_h;
    if (s_font_vis_rows < 1) s_font_vis_rows = 1;

    s_font_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_font_list);
    lv_obj_set_pos(s_font_list, 0, ui_content_y());
    lv_obj_set_size(s_font_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_font_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 找到当前字体大小的索引 */
    s_font_sel = 0;
    for (int i = 0; i < FONT_OPTION_COUNT; i++) {
        if (s_font_sizes[i] == st->font_size) {
            s_font_sel = i;
            break;
        }
    }
    s_font_scroll = 0;

    font_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void font_settings_destroy(void)
{
    ESP_LOGI(TAG, "Font settings destroy");
    s_font_list = NULL;
    memset(s_font_labels, 0, sizeof(s_font_labels));
}

static bool font_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();

    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (key == KEY_UP) {
        s_font_sel = (s_font_sel - 1 + s_font_total) % s_font_total;
        font_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_font_sel = (s_font_sel + 1) % s_font_total;
        font_rebuild_visible();
        return true;
    }

    if (key == KEY_A) {
        if (s_font_sel < FONT_OPTION_COUNT) {
            /* 选择字体大小 */
            int new_size = s_font_sizes[s_font_sel];
            if (new_size != st->font_size) {
                st->font_size = new_size;
                ESP_LOGI(TAG, "Font size changed to: %dpx", new_size);
                font_rebuild_visible();
            }
        }
        return true;
    }

    return false;
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_font_settings_callbacks = {
    .init = font_settings_init,
    .destroy = font_settings_destroy,
    .on_key = font_settings_on_key,
};