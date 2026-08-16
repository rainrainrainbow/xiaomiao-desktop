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
#include "fonts/lv_freetype_font.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_SETTINGS";

/* ========== 设置应用（可滚动列表） ========== */
#define SETTINGS_HDR_H  12
#define SETTINGS_ROW_H  14   /* 每行高度（与字体14px匹配） */

/* 设置项：11项，分组显示 */
static const char *s_settings_items[] = {
    "亮度",       // 0 - 显示
    "主题",       // 1 - 显示
    "音量",       // 2 - 声音（改为音量百分比）
    "WiFi",       // 3 - 网络
    "布局",       // 4 - 桌面
    "字体",       // 5 - 显示（字体大小）
    "应用管理",   // 6 - 二级页面
    "关于系统",   // 7 - 二级页面
    "恢复默认",   // 8 - 操作
    "保存并退出", // 9 - 操作
    "返回Loader", // 10 - 操作（重启进入下载模式）
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

/* 可见区域 */
#define SETTINGS_LIST_Y     (14 + SETTINGS_HDR_H)  /* 标题栏下方 */
#define SETTINGS_LIST_H     (LCD_V_RES - SETTINGS_LIST_Y - DOCK_H)
#define SETTINGS_VIS_ROWS   (SETTINGS_LIST_H / SETTINGS_ROW_H)

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[11] = {0};
static int s_settings_sel = 0;
static int s_settings_scroll = 0;  /* 滚动偏移（行数） */

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    const char *items[] = {
        "亮度", "主题", "音量", "WiFi", "布局", "字体", "应用管理", "关于系统", "恢复默认", "保存并退出", "返回Loader"
    };
    char buf[64];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", items[0], st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", items[1],
                     st->theme == THEME_DARK ? "深色" : "浅色"); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %d%%", items[2], st->volume); break;
    case 3: snprintf(buf, sizeof(buf), "%s: %s", items[3], st->wifi_on ? "开" : "关"); break;
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
    case 7: snprintf(buf, sizeof(buf), "%s", items[7]); break;
    case 8: snprintf(buf, sizeof(buf), "%s", items[8]); break;
    default: snprintf(buf, sizeof(buf), "%s", items[idx]); break;
    }
    lv_label_set_text(s_settings_labels[idx], buf);
}

/* 重建所有可见行的位置和显示状态 */
static void settings_rebuild_visible(void)
{
    if (!s_settings_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    int vis_rows = SETTINGS_VIS_ROWS;
    if (vis_rows < 1) vis_rows = 1;
    /* 清除所有子对象 */
    lv_obj_clean(s_settings_list);
    memset(s_settings_labels, 0, sizeof(s_settings_labels));
    /* 只创建可见范围内的行 */
    for (int i = 0; i < vis_rows && (s_settings_scroll + i) < SETTINGS_ITEM_COUNT; i++) {
        int idx = s_settings_scroll + i;
        lv_obj_t *row = lv_obj_create(s_settings_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * SETTINGS_ROW_H);
        lv_obj_set_size(row, LCD_H_RES, SETTINGS_ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_settings_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[idx] = lbl;
        settings_refresh_label(idx);
    }
}

static void settings_scroll_to_sel(void)
{
    /* 确保选中项在可见范围内 */
    if (s_settings_sel < s_settings_scroll) {
        s_settings_scroll = s_settings_sel;
    } else if (s_settings_sel >= s_settings_scroll + SETTINGS_VIS_ROWS) {
        s_settings_scroll = s_settings_sel - SETTINGS_VIS_ROWS + 1;
    }
    /* 限制滚动范围 */
    if (s_settings_scroll > SETTINGS_ITEM_COUNT - SETTINGS_VIS_ROWS) {
        s_settings_scroll = SETTINGS_ITEM_COUNT - SETTINGS_VIS_ROWS;
    }
    if (s_settings_scroll < 0) s_settings_scroll = 0;
}

static void settings_init(void *data)
{
    ESP_LOGI(TAG, "Settings app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "设置");
    s_settings_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_settings_list);
    lv_obj_set_pos(s_settings_list, 0, SETTINGS_LIST_Y);
    lv_obj_set_size(s_settings_list, LCD_H_RES, SETTINGS_LIST_H);
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
        int delta = (key == KEY_LEFT) ? -1 : 1;
        switch (s_settings_sel) {
        case 0:
            st->brightness += delta * 10;
            if (st->brightness < 10) st->brightness = 10;
            if (st->brightness > 100) st->brightness = 100;
            drv_backlight_set_brightness(st->brightness);
            break;
        case 1:
            st->theme = (st->theme == THEME_DARK) ? THEME_LIGHT : THEME_DARK;
            ui_theme_set(st->theme);
            break;
        case 2:
            st->volume += delta * 10;
            if (st->volume < 0) st->volume = 0;
            if (st->volume > 100) st->volume = 100;
            break;
        case 3:
            st->wifi_on = !st->wifi_on;
            break;
        case 4:
            st->layout = (st->layout == 0) ? 1 : 0;
            break;
        case 5: {
            /* 字体大小：14 → 16 → 20 → 24 → 14 循环 */
            int sizes[] = {14, 16, 20, 24};
            int cur = st->font_size;
            int next = 14;
            if (cur == 14) next = 16;
            else if (cur == 16) next = 20;
            else if (cur == 20) next = 24;
            else if (cur == 24) next = 14;
            st->font_size = next;
            ESP_LOGI(TAG, "Font size changed: %dpx -> %dpx", cur, next);
            break;
        }
        case 6:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
            return true;
        case 7:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_about_callbacks, NULL);
            return true;
        case 8:
            st->brightness = 50; st->volume = 50; st->theme = THEME_DARK;
            st->sound_on = true; st->wifi_on = false; st->layout = 0; st->font_size = 14;
            drv_backlight_set_brightness(st->brightness);
            ui_theme_set(st->theme);
            ESP_LOGI(TAG, "Settings reset to defaults");
            settings_rebuild_visible();
            return true;
        case 9:
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            ui_stack_pop();
            return true;
        case 10:
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
#define ABOUT_ROW_H     14
#define ABOUT_LIST_Y    26
#define ABOUT_LIST_H    (LCD_V_RES - ABOUT_LIST_Y - DOCK_H)
#define ABOUT_VIS_ROWS  (ABOUT_LIST_H / ABOUT_ROW_H)
#define ABOUT_TOTAL     10

static lv_obj_t *s_about_obj = NULL;
static int s_about_scroll = 0;

static void about_rebuild_visible(void)
{
    if (!s_about_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(s_about_obj);
    int vis = ABOUT_VIS_ROWS;
    if (vis < 1) vis = 1;
    for (int i = 0; i < vis && (s_about_scroll + i) < ABOUT_TOTAL; i++) {
        int idx = s_about_scroll + i;
        lv_obj_t *row = lv_obj_create(s_about_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * ABOUT_ROW_H);
        lv_obj_set_size(row, LCD_H_RES, ABOUT_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        char buf[48];
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
        case 8: snprintf(buf, sizeof(buf), "内存: %d KB 空闲",
                         heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024); break;
        case 9: {
            extern uint8_t _rodata_start, _rodata_end, _data_start, _data_end, _bss_start, _bss_end;
            uint32_t flash_size = (uint32_t)&_rodata_end - (uint32_t)&_rodata_start
                                + (uint32_t)&_data_end - (uint32_t)&_data_start;
            snprintf(buf, sizeof(buf), "固件: %lu KB", (unsigned long)(flash_size / 1024));
            break;
        }
        default: buf[0] = '\0'; break;
        }
        lv_label_set_text(lbl, buf);
    }
}

static void about_init(void *data)
{
    ESP_LOGI(TAG, "About page init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "关于系统");
    s_about_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_about_obj);
    lv_obj_set_pos(s_about_obj, 0, ABOUT_LIST_Y);
    lv_obj_set_size(s_about_obj, LCD_H_RES, ABOUT_LIST_H);
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
        if (s_about_scroll + ABOUT_VIS_ROWS < ABOUT_TOTAL) {
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