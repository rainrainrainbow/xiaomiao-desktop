/**
 * @file app_settings.c
 * @brief 设置应用 + 关于系统子页面
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_settings_callbacks。
 * 参考 LiClock 的 App 架构设计，每个 App 独立文件。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "system/sys_nvs.h"
#include "driver/drv_backlight.h"
#include "driver/drv_battery.h"
#include "poincare/runtime.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_SETTINGS";

/* ========== 设置应用（可滚动列表） ========== */
/* 行高根据字体大小动态计算，在 settings_init 中设置 */
static int s_settings_row_h = 14;
static int s_settings_vis_rows = 6;

/* 设置项：18项，分组显示 */
static const char *s_settings_items[] = {
    "亮度",       // 0 - 显示 → 二级页面  (deprecated, use lang_get)
    "主题",       // 1
    "音量",       // 2
    "WiFi",       // 3
    "布局",       // 4
    "字体",       // 5
    "字库选择",   // 6
    "语言",       // 7
    "声音",       // 8
    "音频输出",   // 9
    "屏幕超时",   // 10
    "日期时间",   // 11
    "OTA更新",    // 12
    "应用管理",   // 13
    "关于系统",   // 14
    "恢复默认",   // 15
    "保存并退出", // 16
    "返回Loader", // 17
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

/* 可见区域（列表起始Y由ui_content_y()动态计算） */

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[18] = {0};
static lv_obj_t *s_settings_switch = NULL;  /* 声音开关组件（第6行） */
static int s_settings_sel = 0;
static int s_settings_scroll = 0;  /* 滚动偏移（行数） */

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[64];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_BRIGHTNESS), st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_THEME),
                     st->theme == THEME_DARK ? lang_get(STR_THEME_DARK) : lang_get(STR_THEME_LIGHT)); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_VOLUME), st->volume); break;
    case 3: snprintf(buf, sizeof(buf), "%s", lang_get(STR_WIFI)); break;
    case 4: snprintf(buf, sizeof(buf), "%s: %s",
                     lang_get(STR_LAYOUT), st->layout == 0 ? lang_get(STR_LAYOUT_3COL) : lang_get(STR_LAYOUT_2COL)); break;
    case 5: {
        const char *size_str = "14px";
        if (st->font_size == 16) size_str = "16px";
        else if (st->font_size == 20) size_str = "20px";
        else if (st->font_size == 24) size_str = "24px";
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_FONT), size_str);
        break;
    }
    case 6: {
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_FONT_SOURCE),
                 st->font_source == 0 ? "FreeType" : lang_get(STR_FONT_SOURCE_BUILTIN));
        break;
    }
    case 7: {
        /* 语言：显示当前语言 */
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_LANGUAGE),
                 lang_get_current() == LANG_ZH ? lang_get(STR_LANGUAGE_ZH) : lang_get(STR_LANGUAGE_EN));
        break;
    }
    case 8: snprintf(buf, sizeof(buf), "%s", lang_get(STR_SOUND)); break;
    case 9: snprintf(buf, sizeof(buf), "%s", lang_get(STR_AUDIO_OUTPUT)); break;
    case 10: {
        const char *sleep_str = lang_get(STR_SLEEP_NEVER);
        if (st->sleep_timeout == 30) sleep_str = lang_get(STR_SLEEP_30S);
        else if (st->sleep_timeout == 60) sleep_str = lang_get(STR_SLEEP_60S);
        else if (st->sleep_timeout == 120) sleep_str = lang_get(STR_SLEEP_2M);
        else if (st->sleep_timeout == 300) sleep_str = lang_get(STR_SLEEP_5M);
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_SLEEP_TIMEOUT), sleep_str);
        break;
    }
    case 11: snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATE_TIME)); break;
    case 12: snprintf(buf, sizeof(buf), "%s", lang_get(STR_APP_MANAGER)); break;
    case 13: snprintf(buf, sizeof(buf), "%s", lang_get(STR_ABOUT)); break;
    default: {
        const char *def_items[] = {
            lang_get(STR_BRIGHTNESS), lang_get(STR_THEME), lang_get(STR_VOLUME), lang_get(STR_WIFI),
            lang_get(STR_LAYOUT), lang_get(STR_FONT), lang_get(STR_FONT_SOURCE), lang_get(STR_LANGUAGE),
            lang_get(STR_SOUND), lang_get(STR_AUDIO_OUTPUT), lang_get(STR_SLEEP_TIMEOUT),
            lang_get(STR_DATE_TIME), lang_get(STR_APP_MANAGER), lang_get(STR_ABOUT),
            lang_get(STR_RESET_DEFAULT), lang_get(STR_SAVE_EXIT), lang_get(STR_RETURN_LOADER)
        };
        snprintf(buf, sizeof(buf), "%s", idx >= 0 && idx < 17 ? def_items[idx] : "");
        break;
    }
    }
    lv_label_set_text(s_settings_labels[idx], buf);
}

/* 重建所有可见行的位置和显示状态 */
static void settings_rebuild_visible(void)
{
    if (!s_settings_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int vis_rows = s_settings_vis_rows;
    if (vis_rows < 1) vis_rows = 1;
    /* 清除所有子对象 */
    lv_obj_clean(s_settings_list);
    memset(s_settings_labels, 0, sizeof(s_settings_labels));
    s_settings_switch = NULL;
    /* 只创建可见范围内的行 */
    for (int i = 0; i < vis_rows && (s_settings_scroll + i) < SETTINGS_ITEM_COUNT; i++) {
        int idx = s_settings_scroll + i;
        lv_obj_t *row = lv_obj_create(s_settings_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_settings_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_settings_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_settings_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        /* 设置页面的字体根据 font_size 自适应 */
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 12);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[idx] = lbl;
        settings_refresh_label(idx);

        /* 第8行（声音开关）：添加LVGL开关组件 */
        if (idx == 8) {
            lv_obj_t *sw = lv_switch_create(row);
            lv_obj_remove_style_all(sw);
            /* 开关背景 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->border), 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(sw, 8, 0);
            /* 开关指示器（开启时填充色） */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->text), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(sw, 8, LV_PART_INDICATOR);
            /* 开关旋钮 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->bg), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
            lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
            lv_obj_set_size(sw, 36, s_settings_row_h - 4);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
            /* 设置开关状态 */
            if (st->sound_on) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
            }
            s_settings_switch = sw;
        }
    }
}

static void settings_scroll_to_sel(void)
{
    /* 确保选中项在可见范围内 */
    if (s_settings_sel < s_settings_scroll) {
        s_settings_scroll = s_settings_sel;
    } else if (s_settings_sel >= s_settings_scroll + s_settings_vis_rows) {
        s_settings_scroll = s_settings_sel - s_settings_vis_rows + 1;
    }
    /* 限制滚动范围 */
    if (s_settings_scroll > SETTINGS_ITEM_COUNT - s_settings_vis_rows) {
        s_settings_scroll = SETTINGS_ITEM_COUNT - s_settings_vis_rows;
    }
    if (s_settings_scroll < 0) s_settings_scroll = 0;
}

static void settings_init(void *data)
{
    ESP_LOGI(TAG, "Settings app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_SETTINGS));
    
    /* 根据字体大小动态计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_settings_row_h = font_px + 2;  /* 字体高度 + 2px间距 */
    /* 可见行数根据实际内容区高度（ui_content_y）计算 */
    s_settings_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_settings_row_h;
    if (s_settings_vis_rows < 1) s_settings_vis_rows = 1;
    
    /* 列表起始位置：状态栏下方 */
    lv_coord_t list_y = ui_content_y();
    
    s_settings_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_settings_list);
    lv_obj_set_pos(s_settings_list, 0, list_y);
    lv_obj_set_size(s_settings_list, LCD_H_RES, LCD_V_RES - list_y - DOCK_H);
    lv_obj_clear_flag(s_settings_list, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_sel = 0;
    s_settings_scroll = 0;
    settings_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void settings_activate(void)
{
    ESP_LOGI(TAG, "Settings app activate");
    ui_statusbar_set_title(lang_get(STR_SETTINGS));
    settings_rebuild_visible();
}

static void settings_destroy(void)
{
    ESP_LOGI(TAG, "Settings app destroy");
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) s_settings_labels[i] = NULL;
    s_settings_list = NULL;
    s_settings_switch = NULL;
}

static bool settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) {
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            sys_nvs_save_font_source(st->font_source);
            ui_stack_pop();
        }
        return true;
    }
    if (key == KEY_UP) {
        s_settings_sel = (s_settings_sel - 1 + SETTINGS_ITEM_COUNT) % SETTINGS_ITEM_COUNT;
        settings_scroll_to_sel();
        settings_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_settings_sel = (s_settings_sel + 1) % SETTINGS_ITEM_COUNT;
        settings_scroll_to_sel();
        settings_rebuild_visible();
        return true;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_A) {
        switch (s_settings_sel) {
        case 0:
            /* 亮度 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_brightness_settings_callbacks, NULL);
                return true;
            }
            break;
        case 1:
            /* 主题 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_theme_settings_callbacks, NULL);
                return true;
            }
            break;
        case 2:
            /* 音量 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_volume_settings_callbacks, NULL);
                return true;
            }
            break;
        case 3:
            /* WiFi → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_wifi_settings_callbacks, NULL);
                return true;
            }
            break;
        case 4:
            /* 布局 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_layout_settings_callbacks, NULL);
                return true;
            }
            break;
        case 5:
            /* 字体 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_font_settings_callbacks, NULL);
                return true;
            }
            break;
        case 6:
            /* 字库选择 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_font_source_settings_callbacks, NULL);
                return true;
            }
            break;
        case 7:
            /* 语言 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_language_settings_callbacks, NULL);
                return true;
            }
            break;
        case 8:
            /* 声音开关（含lv_switch） */
            if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_A) {
                st->sound_on = !st->sound_on;
                if (s_settings_switch) {
                    if (st->sound_on) {
                        lv_obj_add_state(s_settings_switch, LV_STATE_CHECKED);
                    } else {
                        lv_obj_clear_state(s_settings_switch, LV_STATE_CHECKED);
                    }
                }
            }
            break;
        case 9:
            /* 音频输出 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_audio_settings_callbacks, NULL);
                return true;
            }
            break;
        case 10:
            /* 屏幕超时 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_sleep_settings_callbacks, NULL);
                return true;
            }
            break;
        case 11:
            /* 日期时间 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_datetime_settings_callbacks, NULL);
                return true;
            }
            break;
        case 12:
            /* OTA更新 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_ota_callbacks, NULL);
                return true;
            }
            break;
        case 13:
            /* 应用管理 → 二级页面 */
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
            return true;
        case 14:
            /* 关于系统 → 二级页面 */
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_about_callbacks, NULL);
            return true;
        case 15:
            /* 恢复默认 */
            st->brightness = 50; st->volume = 50; st->theme = THEME_DARK;
            st->sound_on = true; st->wifi_on = false; st->layout = 0; st->font_size = 14;
            st->sleep_timeout = 60; st->font_source = 0;
            /* 清空用户选择的字体路径并立即持久化，确保重启后恢复默认字体 */
            sys_nvs_save_font_path(0);
            sys_nvs_save_font_source(0);
            drv_backlight_set_brightness(st->brightness);
            ui_theme_set(st->theme);
            ESP_LOGI(TAG, "Settings reset to defaults");
            settings_rebuild_visible();
            return true;
        case 16:
            /* 保存并退出 */
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            sys_nvs_save_font_source(st->font_source);
            ui_stack_pop();
            return true;
        case 17:
            /* 返回Loader（重启进入下载模式） */
            ESP_LOGI(TAG, "Returning to loader (download mode)...");
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            sys_nvs_save_font_source(st->font_source);
            esp_restart();
            return true;
        }
        settings_refresh_label(s_settings_sel);
        return true;
    }
    return false;
}

/* ========== 关于系统页面（可滚动） ========== */
/* 固定信息项数量（不含分区列表）
 * 0系统 1版本 2构建 3芯片 4屏幕 5Python 6字体引擎 7电池
 * 8DRAM 9PSRAM 10IRAM 11通用堆 12运行时间 13固件大小 */
#define ABOUT_ITEMS_FIXED   14
/* 行高根据字体大小动态计算，在 about_init 中设置 */
static int s_about_row_h = 14;
static int s_about_vis_rows = 6;

static lv_obj_t *s_about_obj = NULL;
static int s_about_scroll = 0;
static int s_about_total = ABOUT_ITEMS_FIXED;  /* 动态调整，包含分区数 */

/* 获取分区总数 */
static int about_get_partition_count(void)
{
    int count = 0;
    esp_partition_iterator_t iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iter) {
        count++;
        /* 注意：esp_partition_next() 内部会释放传入的iterator，
         * 到达末尾返回NULL时已自动释放，不能再手动 release，否则双重释放崩溃 */
        iter = esp_partition_next(iter);
    }
    return count;
}

/* 获取第 index 个分区的指针 */
static const esp_partition_t *about_get_partition_at(int index)
{
    int count = 0;
    esp_partition_iterator_t iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    const esp_partition_t *result = NULL;
    while (iter) {
        if (count == index) {
            result = esp_partition_get(iter);
            /* 注意：esp_partition_next() 内部会释放传入的iterator */
            esp_partition_iterator_release(iter);
            return result;
        }
        count++;
        /* esp_partition_next() 到达末尾返回NULL时已自动释放，不能再次release */
        iter = esp_partition_next(iter);
    }
    return NULL;
}

static void about_rebuild_visible(void)
{
    if (!s_about_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_about_obj);
    int vis = s_about_vis_rows;
    if (vis < 1) vis = 1;
    for (int i = 0; i < vis && (s_about_scroll + i) < s_about_total; i++) {
        int idx = s_about_scroll + i;
        lv_obj_t *row = lv_obj_create(s_about_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_about_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_about_row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        /* 关于页面的字体根据 font_size 自适应 */
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 12);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        char buf[64];
        if (idx < ABOUT_ITEMS_FIXED) {
            /* 固定信息项 */
            switch (idx) {
            case 0: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_SYSTEM), "XiaoMiaoOS"); break;
            case 1: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_VERSION), XIAOMIAO_VERSION); break;
            case 2: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_BUILD), XIAOMIAO_BUILD); break;
            case 3: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_CHIP), "ESP32-WROVER-B"); break;
            case 4: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_SCREEN), "ST7735 160x128"); break;
            case 5: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_PYTHON),
                             poincare_runtime_is_ready() ? "Ready" : "Not Ready"); break;
            case 6: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_FONT_ENGINE),
                             lv_freetype_font_is_ready() ? "FreeType" : "Built-in"); break;
            case 7: {
                float vbat = drv_battery_get_voltage();
                if (vbat >= BAT_MIN_VALID_V) {
                    int pct = drv_battery_get_percent(vbat);
                    snprintf(buf, sizeof(buf), "%s: %d%% (%.2fV)", lang_get(STR_BATTERY), pct, vbat);
                } else {
                    snprintf(buf, sizeof(buf), "%s: N/A", lang_get(STR_BATTERY));
                }
                break;
            }
            case 8: {
                /* DRAM: 已用/总容量 + 最低空闲 + 最大连续块 */
                size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t used_dram = total_dram - free_dram;
                size_t minfree_dram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                size_t lrg_dram = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                snprintf(buf, sizeof(buf), "DRAM %lu/%luK 低%lu 大%lu",
                         (unsigned long)(used_dram / 1024),
                         (unsigned long)(total_dram / 1024),
                         (unsigned long)(minfree_dram / 1024),
                         (unsigned long)(lrg_dram / 1024));
                break;
            }
            case 9: {
                /* PSRAM: 已用/总容量 + 最低空闲 + 最大连续块 */
                size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                if (total_psram > 0) {
                    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    size_t used_psram = total_psram - free_psram;
                    size_t minfree_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
                    size_t lrg_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
                    snprintf(buf, sizeof(buf), "%s %lu/%luK 低%lu 大%lu",
                             lang_get(STR_PSRAM),
                             (unsigned long)(used_psram / 1024),
                             (unsigned long)(total_psram / 1024),
                             (unsigned long)(minfree_psram / 1024),
                             (unsigned long)(lrg_psram / 1024));
                } else {
                    snprintf(buf, sizeof(buf), "%s N/A", lang_get(STR_PSRAM));
                }
                break;
            }
            case 10: {
                /* IRAM (指令内存): 总容量/空闲 + 最低空闲 + 最大连续块 */
                size_t total_iram = heap_caps_get_total_size(MALLOC_CAP_EXEC);
                size_t free_iram = heap_caps_get_free_size(MALLOC_CAP_EXEC);
                size_t used_iram = total_iram - free_iram;
                size_t minfree_iram = heap_caps_get_minimum_free_size(MALLOC_CAP_EXEC);
                size_t lrg_iram = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
                snprintf(buf, sizeof(buf), "%s %lu/%luK 低%lu 大%lu",
                         lang_get(STR_IRAM),
                         (unsigned long)(used_iram / 1024),
                         (unsigned long)(total_iram / 1024),
                         (unsigned long)(minfree_iram / 1024),
                         (unsigned long)(lrg_iram / 1024));
                break;
            }
            case 11: {
                /* 通用堆: 总容量/空闲 + 最大连续块；DMA可用 */
                size_t total8 = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                size_t lrg8 = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
                snprintf(buf, sizeof(buf), "%s %lu/%luK 大%lu DMA%lu",
                         lang_get(STR_MEMORY),
                         (unsigned long)(free8 / 1024),
                         (unsigned long)(total8 / 1024),
                         (unsigned long)(lrg8 / 1024),
                         (unsigned long)(free_dma / 1024));
                break;
            }
            case 12: {
                /* 运行时间 */
                uint64_t us = esp_timer_get_time();
                uint32_t sec = (uint32_t)(us / 1000000);
                uint32_t h = sec / 3600;
                uint32_t m = (sec % 3600) / 60;
                snprintf(buf, sizeof(buf), "%s: %luh%lum", lang_get(STR_UPTIME), (unsigned long)h, (unsigned long)m);
                break;
            }
            case 13: {
                /* 固件大小 */
                extern uint8_t _rodata_start, _rodata_end, _data_start, _data_end;
                uint32_t flash_size = (uint32_t)&_rodata_end - (uint32_t)&_rodata_start
                                    + (uint32_t)&_data_end - (uint32_t)&_data_start;
                snprintf(buf, sizeof(buf), "%s:%luK Flash:4MB", lang_get(STR_FIRMWARE),
                         (unsigned long)(flash_size / 1024));
                break;
            }
            default: buf[0] = '\0'; break;
            }
        } else {
            /* 分区信息行 */
            int part_idx = idx - ABOUT_ITEMS_FIXED;
            const esp_partition_t *part = about_get_partition_at(part_idx);
            if (part) {
                char type_c = (part->type == ESP_PARTITION_TYPE_APP) ? 'A' : 'D';
                snprintf(buf, sizeof(buf), " %c %s:%luK",
                         type_c, part->label,
                         (unsigned long)(part->size / 1024));
            } else {
                buf[0] = '\0';
            }
        }
        lv_label_set_text(lbl, buf);
    }
}

static void about_init(void *data)
{
    ESP_LOGI(TAG, "About page init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_ABOUT));

    /* 根据字体大小动态计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_about_row_h = font_px + 2;  /* 字体高度 + 2px间距 */
    /* 可见行数根据实际内容区高度（ui_content_y）计算 */
    s_about_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_about_row_h;
    if (s_about_vis_rows < 1) s_about_vis_rows = 1;

    /* 动态计算总行数：固定项 + 分区数 */
    s_about_total = ABOUT_ITEMS_FIXED + about_get_partition_count();

    /* 列表起始位置：状态栏下方 */
    lv_coord_t list_y = ui_content_y();

    s_about_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_about_obj);
    lv_obj_set_pos(s_about_obj, 0, list_y);
    lv_obj_set_size(s_about_obj, LCD_H_RES, LCD_V_RES - list_y - DOCK_H);
    lv_obj_clear_flag(s_about_obj, LV_OBJ_FLAG_SCROLLABLE);
    s_about_scroll = 0;
    about_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void about_destroy(void)
{
    ESP_LOGI(TAG, "About page destroy");
    s_about_obj = NULL;
    s_about_scroll = 0;
}

static bool about_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    if (key == KEY_UP) {
        if (s_about_scroll > 0) {
            s_about_scroll--;
            about_rebuild_visible();
        }
        return true;
    }
    if (key == KEY_DOWN) {
        if (s_about_scroll + s_about_vis_rows < s_about_total) {
            s_about_scroll++;
            about_rebuild_visible();
        }
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_settings_callbacks = {
    .init = settings_init,
    .activate = settings_activate,
    .destroy = settings_destroy,
    .on_key = settings_on_key,
};

const page_callbacks_t g_about_callbacks = {
    .init = about_init,
    .destroy = about_destroy,
    .on_key = about_on_key,
};