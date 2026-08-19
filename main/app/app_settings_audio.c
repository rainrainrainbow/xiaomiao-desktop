/**
 * @file app_settings_audio.c
 * @brief 音频输出设置页面 - 选择输出设备、自动/手动模式
 */
#include "ui/ui_framework.h"
#include "driver/drv_audio_output.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "AUDIO_SETTINGS";

/* ========== 页面状态 ========== */
static int s_audio_selected = 0;
static int s_audio_scroll = 0;
static lv_obj_t *s_audio_rows[AUDIO_OUT_MAX + 2] = {0};
static lv_obj_t *s_audio_mode_label = NULL;
static lv_obj_t *s_audio_volume_bar = NULL;

/* ========== 刷新页面 ========== */
static void audio_settings_refresh(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *state = ui_state_get();
    int font_px = state->font_size;
    (void)font_px;
    
    /* 获取设备列表 */
    audio_device_info_t devs[AUDIO_OUT_MAX];
    int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
    
    /* 更新设备行 */
    for (int i = 0; i < dev_count && i < AUDIO_OUT_MAX; i++) {
        if (!s_audio_rows[i]) continue;
        
        /* 更新选中状态 */
        if (i == s_audio_selected) {
            lv_obj_set_style_bg_color(s_audio_rows[i], lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(s_audio_rows[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(s_audio_rows[i], LV_OPA_TRANSP, 0);
        }
        
        /* 更新设备名称和状态 */
        lv_obj_t *name_lbl = lv_obj_get_child(s_audio_rows[i], 0);
        if (name_lbl) {
            char buf[32];
            const char *status = devs[i].available ? "" : lang_get(STR_AUDIO_UNAVAILABLE);
            snprintf(buf, sizeof(buf), "%s%s", devs[i].name, status);
            lv_label_set_text(name_lbl, buf);
            
            /* 不可用设备用灰色 */
            lv_color_t text_color = devs[i].available ? 
                lv_color_hex(colors->text) : lv_color_hex(colors->text_dim);
            lv_obj_set_style_text_color(name_lbl, text_color, 0);
        }
        
        /* 更新当前标记 */
        lv_obj_t *mark_lbl = lv_obj_get_child(s_audio_rows[i], 1);
        if (mark_lbl) {
            if (devs[i].is_default) {
                lv_label_set_text(mark_lbl, "●");
                lv_obj_set_style_text_color(mark_lbl, lv_color_hex(0x22C55E), 0);
            } else {
                lv_label_set_text(mark_lbl, "○");
                lv_obj_set_style_text_color(mark_lbl, lv_color_hex(colors->text_dim), 0);
            }
        }
    }
    
    /* 更新模式标签 */
    if (s_audio_mode_label) {
        bool auto_mode = audio_output_is_auto_mode();
        lv_label_set_text(s_audio_mode_label, auto_mode ? lang_get(STR_AUDIO_MODE_AUTO) : lang_get(STR_AUDIO_MODE_MANUAL));
    }
    
    /* 更新音量条 */
    if (s_audio_volume_bar) {
        uint8_t vol = audio_output_get_volume();
        lv_bar_set_value(s_audio_volume_bar, vol, LV_ANIM_OFF);
    }
}

/* ========== 页面回调 ========== */
static void audio_settings_init(void *data)
{
    ESP_LOGI(TAG, "Audio settings init");
    
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *state = ui_state_get();
    int font_px = state->font_size;
    int row_h = font_px + 4;
    
    /* 清屏 */
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    /* 状态栏 */
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_AUDIO_OUTPUT));
    
    /* 内容区 */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, ui_content_y());
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 获取设备列表 */
    audio_device_info_t devs[AUDIO_OUT_MAX];
    int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
    
    /* 创建设备行 */
    int y = 0;
    for (int i = 0; i < dev_count && i < AUDIO_OUT_MAX; i++) {
        lv_obj_t *row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        
        /* 设备名称 */
        lv_obj_t *name_lbl = lv_label_create(row);
        char buf[32];
        const char *status = devs[i].available ? "" : lang_get(STR_AUDIO_UNAVAILABLE);
        snprintf(buf, sizeof(buf), "%s%s", devs[i].name, status);
        lv_label_set_text(name_lbl, buf);
        lv_obj_set_style_text_font(name_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 8, 0);
        
        /* 当前标记 */
        lv_obj_t *mark_lbl = lv_label_create(row);
        lv_label_set_text(mark_lbl, devs[i].is_default ? "●" : "○");
        lv_obj_set_style_text_font(mark_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(mark_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
        
        s_audio_rows[i] = row;
        y += row_h;
    }
    
    /* 分隔线 */
    y += 2;
    lv_obj_t *sep = lv_obj_create(content);
    lv_obj_remove_style_all(sep);
    lv_obj_set_pos(sep, 8, y);
    lv_obj_set_size(sep, LCD_H_RES - 16, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
    y += 4;
    
    /* 模式选择行 */
    lv_obj_t *mode_row = lv_obj_create(content);
    lv_obj_remove_style_all(mode_row);
    lv_obj_set_pos(mode_row, 0, y);
    lv_obj_set_size(mode_row, LCD_H_RES, row_h);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *mode_title = lv_label_create(mode_row);
    lv_label_set_text(mode_title, lang_get(STR_AUDIO_MODE));
    lv_obj_set_style_text_font(mode_title, lv_font_cn_get(font_px), 0);
    lv_obj_align(mode_title, LV_ALIGN_LEFT_MID, 8, 0);
    
    s_audio_mode_label = lv_label_create(mode_row);
    bool auto_mode = audio_output_is_auto_mode();
    lv_label_set_text(s_audio_mode_label, auto_mode ? lang_get(STR_AUDIO_MODE_AUTO) : lang_get(STR_AUDIO_MODE_MANUAL));
    lv_obj_set_style_text_font(s_audio_mode_label, lv_font_cn_get(font_px), 0);
    lv_obj_set_style_text_color(s_audio_mode_label, lv_color_hex(colors->sel_bg), 0);
    lv_obj_align(s_audio_mode_label, LV_ALIGN_RIGHT_MID, -8, 0);
    
    s_audio_rows[dev_count] = mode_row;  // 存储模式行
    y += row_h + 4;
    
    /* 音量调节行 */
    lv_obj_t *vol_row = lv_obj_create(content);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_pos(vol_row, 0, y);
    lv_obj_set_size(vol_row, LCD_H_RES, row_h);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *vol_title = lv_label_create(vol_row);
    lv_label_set_text(vol_title, lang_get(STR_VOLUME));
    lv_obj_set_style_text_font(vol_title, lv_font_cn_get(font_px), 0);
    lv_obj_align(vol_title, LV_ALIGN_LEFT_MID, 8, 0);
    
    s_audio_volume_bar = lv_bar_create(vol_row);
    lv_obj_set_size(s_audio_volume_bar, 60, 6);
    lv_bar_set_range(s_audio_volume_bar, 0, 100);
    lv_bar_set_value(s_audio_volume_bar, audio_output_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_audio_volume_bar, lv_color_hex(colors->sel_bg), LV_PART_INDICATOR);
    lv_obj_align(s_audio_volume_bar, LV_ALIGN_RIGHT_MID, -8, 0);
    
    s_audio_rows[dev_count + 1] = vol_row;  // 存储音量行
    
    /* 底部提示 */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, lang_get(STR_AUDIO_HINT));
    lv_obj_set_style_text_color(hint, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(hint, lv_font_cn_get(12), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);
    
    /* 重置选择 */
    s_audio_selected = 0;
    s_audio_scroll = 0;
    
    /* 初始刷新 */
    audio_settings_refresh();
}

static void audio_settings_destroy(void)
{
    ESP_LOGI(TAG, "Audio settings destroy");
    for (int i = 0; i < AUDIO_OUT_MAX + 2; i++) {
        s_audio_rows[i] = NULL;
    }
    s_audio_mode_label = NULL;
    s_audio_volume_bar = NULL;
}

static bool audio_settings_on_key(int key)
{
    /* 获取设备列表 */
    audio_device_info_t devs[AUDIO_OUT_MAX];
    int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
    int total_items = dev_count + 2;  // 设备 + 模式 + 音量
    
    if (key == KEY_B) {
        ui_stack_pop();
        return true;
    }
    
    if (key == KEY_UP) {
        if (s_audio_selected > 0) {
            s_audio_selected--;
            audio_settings_refresh();
        }
        return true;
    }
    
    if (key == KEY_DOWN) {
        if (s_audio_selected < total_items - 1) {
            s_audio_selected++;
            audio_settings_refresh();
        }
        return true;
    }
    
    if (key == KEY_A) {
        if (s_audio_selected < dev_count) {
            /* 选择设备 */
            if (devs[s_audio_selected].available) {
                audio_output_set_active(devs[s_audio_selected].type);
                sys_nvs_save_audio_output(devs[s_audio_selected].type);
                audio_settings_refresh();
            }
        } else if (s_audio_selected == dev_count) {
            /* 切换模式 */
            bool auto_mode = audio_output_is_auto_mode();
            audio_output_set_auto_mode(!auto_mode);
            sys_nvs_save_audio_auto(!auto_mode);
            audio_settings_refresh();
        }
        /* 音量行不响应A键 */
        return true;
    }
    
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (s_audio_selected == dev_count + 1) {
            /* 调节音量 */
            uint8_t vol = audio_output_get_volume();
            if (key == KEY_LEFT) {
                vol = (vol >= 10) ? vol - 10 : 0;
            } else {
                vol = (vol <= 90) ? vol + 10 : 100;
            }
            audio_output_set_volume(vol);
            sys_nvs_save_volume(vol);
            audio_settings_refresh();
        } else if (s_audio_selected == dev_count) {
            /* 模式行也响应左右键 */
            bool auto_mode = audio_output_is_auto_mode();
            audio_output_set_auto_mode(!auto_mode);
            sys_nvs_save_audio_auto(!auto_mode);
            audio_settings_refresh();
        }
        return true;
    }
    
    return true;
}

/* ========== 页面注册 ========== */
const page_callbacks_t g_audio_settings_callbacks = {
    .init = audio_settings_init,
    .destroy = audio_settings_destroy,
    .on_key = audio_settings_on_key,
};