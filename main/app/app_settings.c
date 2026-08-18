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
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_SETTINGS";

/* ========== 设置应用（可滚动列表） ========== */
/* 行高根据字体大小动态计算，在 settings_init 中设置 */
static int s_settings_row_h = 14;
static int s_settings_vis_rows = 6;

/* 设置项：15项，分组显示 */
static const char *s_settings_items[] = {
    "亮度",       // 0 - 显示 → 二级页面
    "主题",       // 1 - 显示 → 二级页面
    "音量",       // 2 - 声音 → 二级页面
    "WiFi",       // 3 - 网络 → 二级页面
    "布局",       // 4 - 桌面 → 二级页面
    "字体",       // 5 - 显示 → 二级页面
    "声音",       // 6 - 声音开关（含lv_switch）
    "屏幕超时",   // 7 - 显示 → 二级页面
    "日期时间",   // 8 - 系统 → 二级页面
    "应用管理",   // 9 - 二级页面
    "关于系统",   // 10 - 二级页面
    "恢复默认",   // 11 - 操作
    "保存并退出", // 12 - 操作
    "返回Loader", // 13 - 操作（重启进入下载模式）
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

/* 可见区域（列表起始Y由ui_content_y()动态计算） */

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[14] = {0};
static lv_obj_t *s_settings_switch = NULL;  /* 声音开关组件（第6行） */
static int s_settings_sel = 0;
static int s_settings_scroll = 0;  /* 滚动偏移（行数） */

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    const char *items[] = {
        "亮度", "主题", "音量", "WiFi", "布局", "字体", "声音",
        "屏幕超时", "日期时间", "应用管理", "关于系统", "恢复默认", "保存并退出", "返回Loader"
    };
    char buf[64];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", items[0], st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", items[1],
                     st->theme == THEME_DARK ? "深色" : "浅色"); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %d%%", items[2], st->volume); break;
    case 3: snprintf(buf, sizeof(buf), "%s", items[3]); break;
    case 4: snprintf(buf, sizeof(buf), "%s: %s",
                     items[4], st->layout == 0 ? "3列" : "2列"); break;
    case 5: {
        const char *size_str = "14px";
        if (st->font_size == 16) size_str = "16px";
        else if (st->font_size == 20) size_str = "20px";
        else if (st->font_size == 24) size_str = "24px";
        snprintf(buf, sizeof(buf), "%s: %s", items[5], size_str);
        break;
    }
    case 6: snprintf(buf, sizeof(buf), "%s", items[6]); break;
    case 7: {
        const char *sleep_str = "永不";
        if (st->sleep_timeout == 30) sleep_str = "30秒";
        else if (st->sleep_timeout == 60) sleep_str = "60秒";
        else if (st->sleep_timeout == 120) sleep_str = "2分";
        else if (st->sleep_timeout == 300) sleep_str = "5分";
        snprintf(buf, sizeof(buf), "%s: %s", items[7], sleep_str);
        break;
    }
    case 8: snprintf(buf, sizeof(buf), "%s", items[8]); break;
    case 9: snprintf(buf, sizeof(buf), "%s", items[9]); break;
    case 10: snprintf(buf, sizeof(buf), "%s", items[10]); break;
    case 11: snprintf(buf, sizeof(buf), "%s", items[11]); break;
    default: snprintf(buf, sizeof(buf), "%s", items[idx]); break;
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
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[idx] = lbl;
        settings_refresh_label(idx);

        /* 第6行（声音开关）：添加LVGL开关组件 */
        if (idx == 6) {
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
    ui_statusbar_set_title("设置");
    
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
    /* 重新构建可见行（因为从二级页面返回时可能主题/状态已变） */
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
        case 7:
            /* 屏幕超时 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_sleep_settings_callbacks, NULL);
                return true;
            }
            break;
        case 8:
            /* 日期时间 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_datetime_settings_callbacks, NULL);
                return true;
            }
            break;
        case 9:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
            return true;
        case 10:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_about_callbacks, NULL);
            return true;
        case 11:
            st->brightness = 50; st->volume = 50; st->theme = THEME_DARK;
            st->sound_on = true; st->wifi_on = false; st->layout = 0; st->font_size = 14;
            st->sleep_timeout = 60;
            drv_backlight_set_brightness(st->brightness);
            ui_theme_set(st->theme);
            ESP_LOGI(TAG, "Settings reset to defaults");
            settings_rebuild_visible();
            return true;
        case 12:
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            ui_stack_pop();
            return true;
        case 13:
            ESP_LOGI(TAG, "Returning to loader (download mode)...");
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            esp_restart();
            return true;
        }
        settings_refresh_label(s_settings_sel);
        return true;
    }
    return false;
}

/* ========== 关于系统页面（可滚动） ========== */
/* 固定信息项数量（不含分区列表） */
#define ABOUT_ITEMS_FIXED   13
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
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        char buf[48];

        if (idx < ABOUT_ITEMS_FIXED) {
            /* 固定信息项 */
            switch (idx) {
            case 0: snprintf(buf, sizeof(buf), "系统: 小喵桌面"); break;
            case 1: snprintf(buf, sizeof(buf), "版本: %s", XIAOMIAO_VERSION); break;
            case 2: snprintf(buf, sizeof(buf), "构建: %s", XIAOMIAO_BUILD); break;
            case 3: snprintf(buf, sizeof(buf), "芯片: ESP32-WROVER-B"); break;
            case 4: snprintf(buf, sizeof(buf), "屏幕: ST7735 160x128"); break;
            case 5: snprintf(buf, sizeof(buf), "Python: %s",
                             poincare_runtime_is_ready() ? "就绪" : "未初始化"); break;
            case 6: snprintf(buf, sizeof(buf), "字体: %s",
                             lv_freetype_font_is_ready() ? "FreeType" : "内置"); break;
            case 7: {
                float vbat = drv_battery_get_voltage();
                if (vbat >= BAT_MIN_VALID_V) {
                    int pct = drv_battery_get_percent(vbat);
                    snprintf(buf, sizeof(buf), "电池: %d%% (%.2fV)", pct, vbat);
                } else {
                    snprintf(buf, sizeof(buf), "电池: 未检测到");
                }
                break;
            }
            case 8: {
                /* DRAM: 已用/总容量 */
                size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
                size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                size_t used_dram = total_dram - free_dram;
                snprintf(buf, sizeof(buf), "DRAM:%luK/%luK",
                         (unsigned long)(used_dram / 1024),
                         (unsigned long)(total_dram / 1024));
                break;
            }
            case 9: {
                /* PSRAM: 已用/总容量 */
                size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                if (total_psram > 0) {
                    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                    size_t used_psram = total_psram - free_psram;
                    snprintf(buf, sizeof(buf), "PSRAM:%luK/%luK",
                             (unsigned long)(used_psram / 1024),
                             (unsigned long)(total_psram / 1024));
                } else {
                    snprintf(buf, sizeof(buf), "PSRAM: 未检测到");
                }
                break;
            }
            case 10: {
                /* DMA空闲 + 堆最小空闲 */
                size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
                size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
                snprintf(buf, sizeof(buf), "DMA:%luK 堆谷:%luK",
                         (unsigned long)(free_dma / 1024),
                         (unsigned long)(min_free / 1024));
                break;
            }
            case 11: {
                /* 运行时间 */
                uint64_t us = esp_timer_get_time();
                uint32_t sec = (uint32_t)(us / 1000000);
                uint32_t h = sec / 3600;
                uint32_t m = (sec % 3600) / 60;
                snprintf(buf, sizeof(buf), "运行: %luh%lum", (unsigned long)h, (unsigned long)m);
                break;
            }
            case 12: {
                /* 固件大小 */
                extern uint8_t _rodata_start, _rodata_end, _data_start, _data_end;
                uint32_t flash_size = (uint32_t)&_rodata_end - (uint32_t)&_rodata_start
                                    + (uint32_t)&_data_end - (uint32_t)&_data_start;
                snprintf(buf, sizeof(buf), "固件:%luK Flash:4MB",
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
    ui_statusbar_set_title("关于系统");

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