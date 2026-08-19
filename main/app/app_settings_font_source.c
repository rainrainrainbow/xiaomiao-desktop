/**
 * @file app_settings_font_source.c
 * @brief 字库选择二级页面 - 选择字库来源
 *
 * 提供两种字库来源选择：
 * 0 = FreeType (SD卡) - 需要SD卡中有NotoSansSC字体文件，支持中文渲染
 * 1 = 内置 (英文) - 使用LVGL内置Montserrat字体，仅显示英文
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_FONT_SRC";

/* ========== 字库来源选项 ========== */
#define FONT_SRC_OPTION_COUNT 2
static const int s_font_src_values[FONT_SRC_OPTION_COUNT] = {0, 1};

/* ========== UI状态 ========== */
static lv_obj_t *s_font_src_list = NULL;
static lv_obj_t *s_font_src_labels[FONT_SRC_OPTION_COUNT] = {0};
static lv_obj_t *s_font_src_checkboxes[FONT_SRC_OPTION_COUNT] = {0};
static int s_font_src_sel = 0;
static int s_font_src_scroll = 0;
static int s_font_src_vis_rows = 6;
static int s_font_src_row_h = 14;

/* 刷新复选框状态 */
static void font_src_refresh_checkboxes(void)
{
    ui_state_t *st = ui_state_get();
    for (int i = 0; i < FONT_SRC_OPTION_COUNT; i++) {
        if (s_font_src_checkboxes[i]) {
            if (s_font_src_values[i] == st->font_source) {
                lv_obj_add_state(s_font_src_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_font_src_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void font_src_rebuild_visible(void)
{
    if (!s_font_src_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_font_src_list);
    memset(s_font_src_labels, 0, sizeof(s_font_src_labels));
    memset(s_font_src_checkboxes, 0, sizeof(s_font_src_checkboxes));

    /* 确保选中项在可见范围内 */
    if (s_font_src_sel < s_font_src_scroll) s_font_src_scroll = s_font_src_sel;
    if (s_font_src_sel >= s_font_src_scroll + s_font_src_vis_rows) {
        s_font_src_scroll = s_font_src_sel - s_font_src_vis_rows + 1;
    }
    if (s_font_src_scroll > FONT_SRC_OPTION_COUNT - s_font_src_vis_rows) {
        s_font_src_scroll = FONT_SRC_OPTION_COUNT - s_font_src_vis_rows;
    }
    if (s_font_src_scroll < 0) s_font_src_scroll = 0;

    for (int i = 0; i < s_font_src_vis_rows && (s_font_src_scroll + i) < FONT_SRC_OPTION_COUNT; i++) {
        int idx = s_font_src_scroll + i;
        lv_obj_t *row = lv_obj_create(s_font_src_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_font_src_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_font_src_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (idx == s_font_src_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        /* 使用LVGL复选框组件 */
        lv_obj_t *cb = lv_checkbox_create(row);
        const char *label = (idx == 0) ? lang_get(STR_FONT_SOURCE_FREETYPE) : lang_get(STR_FONT_SOURCE_BUILTIN);
        lv_checkbox_set_text(cb, label);
        lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
        if (s_font_src_values[idx] == st->font_source) {
            lv_obj_add_state(cb, LV_STATE_CHECKED);
        }
        s_font_src_checkboxes[idx] = cb;
        s_font_src_labels[idx] = lv_label_create(row); /* 占位 */
    }
}

/* ========== 页面生命周期 ========== */
static void font_src_settings_init(void *data)
{
    ESP_LOGI(TAG, "Font source settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_FONT_SOURCE));

    /* 计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_font_src_row_h = font_px + 2;
    s_font_src_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_font_src_row_h;
    if (s_font_src_vis_rows < 1) s_font_src_vis_rows = 1;

    s_font_src_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_font_src_list);
    lv_obj_set_pos(s_font_src_list, 0, ui_content_y());
    lv_obj_set_size(s_font_src_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_font_src_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 找到当前字库来源的索引 */
    s_font_src_sel = 0;
    for (int i = 0; i < FONT_SRC_OPTION_COUNT; i++) {
        if (s_font_src_values[i] == st->font_source) {
            s_font_src_sel = i;
            break;
        }
    }
    s_font_src_scroll = 0;

    font_src_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void font_src_settings_destroy(void)
{
    ESP_LOGI(TAG, "Font source settings destroy");
    s_font_src_list = NULL;
    memset(s_font_src_labels, 0, sizeof(s_font_src_labels));
    memset(s_font_src_checkboxes, 0, sizeof(s_font_src_checkboxes));
}

static bool font_src_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();

    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (key == KEY_UP) {
        s_font_src_sel = (s_font_src_sel - 1 + FONT_SRC_OPTION_COUNT) % FONT_SRC_OPTION_COUNT;
        font_src_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_font_src_sel = (s_font_src_sel + 1) % FONT_SRC_OPTION_COUNT;
        font_src_rebuild_visible();
        return true;
    }

    if (key == KEY_A) {
        if (s_font_src_sel < FONT_SRC_OPTION_COUNT) {
            int new_src = s_font_src_values[s_font_src_sel];
            if (new_src != st->font_source) {
                st->font_source = new_src;
                ESP_LOGI(TAG, "Font source changed to: %d", new_src);
                font_src_refresh_checkboxes();
                font_src_rebuild_visible();
            }
        }
        return true;
    }

    return false;
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_font_source_settings_callbacks = {
    .init = font_src_settings_init,
    .destroy = font_src_settings_destroy,
    .on_key = font_src_settings_on_key,
};