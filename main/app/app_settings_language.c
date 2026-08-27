/**
 * @file app_settings_language.c
 * @brief 语言设置二级页面 - 选择中文/English
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_LANG";

#define LANG_OPTION_COUNT 2

static lv_obj_t *s_lang_list = NULL;
static lv_obj_t *s_lang_labels[LANG_OPTION_COUNT + 1] = {0};
static lv_obj_t *s_lang_checkboxes[LANG_OPTION_COUNT] = {0};
static int s_lang_sel = 0;
static int s_lang_scroll = 0;
static int s_lang_vis_rows = 6;
static int s_lang_row_h = 14;
static int s_lang_total = LANG_OPTION_COUNT + 1;

/* 获取语言选项名称 */
static const char* lang_option_name(int idx)
{
    return idx == 0 ? lang_get(STR_LANGUAGE_ZH) : lang_get(STR_LANGUAGE_EN);
}

static void lang_refresh_label(int idx)
{
    if (!s_lang_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];
    if (idx < LANG_OPTION_COUNT) {
        snprintf(buf, sizeof(buf), "%s", lang_option_name(idx));
    } else {
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_CURRENT_VALUE),
                 lang_get_current() == LANG_ZH ? lang_get(STR_LANGUAGE_ZH) : lang_get(STR_LANGUAGE_EN));
    }
    lv_label_set_text(s_lang_labels[idx], buf);
}

/* 刷新复选框状态 */
static void lang_refresh_checkboxes(void)
{
    lang_id_t cur = lang_get_current();
    for (int i = 0; i < LANG_OPTION_COUNT; i++) {
        if (s_lang_checkboxes[i]) {
            if ((i == 0 && cur == LANG_ZH) || (i == 1 && cur == LANG_EN)) {
                lv_obj_add_state(s_lang_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_lang_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void lang_rebuild_visible(void)
{
    if (!s_lang_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_lang_list);
    memset(s_lang_labels, 0, sizeof(s_lang_labels));
    memset(s_lang_checkboxes, 0, sizeof(s_lang_checkboxes));
    s_lang_total = LANG_OPTION_COUNT + 1;
    if (s_lang_sel < s_lang_scroll) s_lang_scroll = s_lang_sel;
    if (s_lang_sel >= s_lang_scroll + s_lang_vis_rows) s_lang_scroll = s_lang_sel - s_lang_vis_rows + 1;
    if (s_lang_scroll > s_lang_total - s_lang_vis_rows) s_lang_scroll = s_lang_total - s_lang_vis_rows;
    if (s_lang_scroll < 0) s_lang_scroll = 0;
    for (int i = 0; i < s_lang_vis_rows && (s_lang_scroll + i) < s_lang_total; i++) {
        int idx = s_lang_scroll + i;
        lv_obj_t *row = lv_obj_create(s_lang_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_lang_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_lang_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_lang_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        if (idx < LANG_OPTION_COUNT) {
            lv_obj_t *cb = lv_checkbox_create(row);
            if (!cb) {
                ESP_LOGE(TAG, "lv_checkbox_create(cb) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_checkbox_set_text(cb, lang_option_name(idx));
            lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
            lang_id_t cur = lang_get_current();
            if ((idx == 0 && cur == LANG_ZH) || (idx == 1 && cur == LANG_EN)) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
            }
            s_lang_checkboxes[idx] = cb;
            lv_obj_t *ph = lv_label_create(row);
            if (ph) s_lang_labels[idx] = ph;
        } else {
            lv_obj_t *lbl = lv_label_create(row);
            if (!lbl) {
                ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
            s_lang_labels[idx] = lbl;
            lang_refresh_label(idx);
        }
    }
}

static void lang_settings_init(void *data)
{
    ESP_LOGI(TAG, "Language settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_LANGUAGE));
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_lang_row_h = font_px + 2;
    s_lang_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_lang_row_h;
    if (s_lang_vis_rows < 1) s_lang_vis_rows = 1;
    s_lang_list = lv_obj_create(scr);
    if (!s_lang_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_lang_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_lang_list);
    lv_obj_set_pos(s_lang_list, 0, ui_content_y());
    lv_obj_set_size(s_lang_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_lang_list, LV_OBJ_FLAG_SCROLLABLE);
    s_lang_sel = lang_get_current();
    s_lang_scroll = 0;
    lang_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void lang_settings_destroy(void)
{
    ESP_LOGI(TAG, "Language settings destroy");
    s_lang_list = NULL;
    memset(s_lang_labels, 0, sizeof(s_lang_labels));
    memset(s_lang_checkboxes, 0, sizeof(s_lang_checkboxes));
}

static bool lang_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP) { s_lang_sel = (s_lang_sel - 1 + s_lang_total) % s_lang_total; lang_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_lang_sel = (s_lang_sel + 1) % s_lang_total; lang_rebuild_visible(); return true; }
    if (key == KEY_A) {
        if (s_lang_sel < LANG_OPTION_COUNT) {
            lang_id_t new_lang = (lang_id_t)s_lang_sel;
            lang_id_t cur = lang_get_current();
            if (new_lang != cur) {
                lang_set(new_lang);
                sys_nvs_save_language(new_lang);
                /* 切换语言后返回桌面触发全 UI 重建，立即生效 */
                ui_stack_back_home();
                return true;
            }
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_language_settings_callbacks = {
    .init = lang_settings_init,
    .destroy = lang_settings_destroy,
    .on_key = lang_settings_on_key,
};