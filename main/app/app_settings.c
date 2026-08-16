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
#include <string.h>

static const char *TAG = "APP_SETTINGS";

/* ========== 设置应用 ========== */
#define SETTINGS_HDR_H  12
#define SETTINGS_ITEM_H  13

/* 设置项：10项，分组显示 */
static const char *s_settings_items[] = {
    "亮度",       // 0 - 显示
    "主题",       // 1 - 显示
    "音量",       // 2 - 声音（改为音量百分比）
    "WiFi",       // 3 - 网络
    "布局",       // 4 - 桌面
    "应用管理",   // 5 - 二级页面
    "关于系统",   // 6 - 二级页面
    "恢复默认",   // 7 - 操作
    "保存并退出", // 8 - 操作
    "返回Loader", // 9 - 操作（重启进入下载模式）
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[10] = {0};
static int s_settings_sel = 0;

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    const char *items[] = {
        "亮度", "主题", "音量", "WiFi", "布局", "应用管理", "关于系统", "恢复默认", "保存并退出", "返回Loader"
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
    case 5: snprintf(buf, sizeof(buf), "%s", items[5]); break;
    case 6: snprintf(buf, sizeof(buf), "%s", items[6]); break;
    case 7: snprintf(buf, sizeof(buf), "%s", items[7]); break;
    default: snprintf(buf, sizeof(buf), "%s", items[idx]); break;
    }
    lv_label_set_text(s_settings_labels[idx], buf);
}

static void settings_apply_highlight(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        if (!s_settings_labels[i]) continue;
        lv_obj_t *parent = lv_obj_get_parent(s_settings_labels[i]);
        if (!parent) continue;
        if (i == s_settings_sel) {
            lv_obj_set_style_bg_color(parent, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
        }
    }
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
    lv_obj_set_pos(s_settings_list, 0, 14 + SETTINGS_HDR_H);
    lv_obj_set_size(s_settings_list, LCD_H_RES, LCD_V_RES - 14 - SETTINGS_HDR_H - DOCK_H);
    lv_obj_clear_flag(s_settings_list, LV_OBJ_FLAG_SCROLLABLE);
    int item_h = (LCD_V_RES - 14 - SETTINGS_HDR_H - DOCK_H) / SETTINGS_ITEM_COUNT;
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_settings_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[i] = lbl;
        settings_refresh_label(i);
    }
    ui_dock_create(scr, 1, 0);
    settings_apply_highlight();
}

static void settings_activate(void)
{
    ESP_LOGI(TAG, "Settings app activate");
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
                                  (int)st->theme, st->wifi_on, st->layout);
            ui_stack_pop();
        }
        return true;
    }
    if (key == KEY_UP) {
        s_settings_sel = (s_settings_sel - 1 + SETTINGS_ITEM_COUNT) % SETTINGS_ITEM_COUNT;
        settings_apply_highlight();
        return true;
    }
    if (key == KEY_DOWN) {
        s_settings_sel = (s_settings_sel + 1) % SETTINGS_ITEM_COUNT;
        settings_apply_highlight();
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
        case 5:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
            return true;
        case 6:
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_about_callbacks, NULL);
            return true;
        case 7:
            st->brightness = 50; st->volume = 50; st->theme = THEME_DARK;
            st->sound_on = true; st->wifi_on = false; st->layout = 0;
            drv_backlight_set_brightness(st->brightness);
            ui_theme_set(st->theme);
            ESP_LOGI(TAG, "Settings reset to defaults");
            for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) settings_refresh_label(i);
            return true;
        case 8:
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout);
            ui_stack_pop();
            return true;
        case 9:
            ESP_LOGI(TAG, "Returning to loader (download mode)...");
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout);
            esp_restart();
            return true;
        }
        settings_refresh_label(s_settings_sel);
        return true;
    }
    return false;
}

/* ========== 关于系统页面 ========== */
static lv_obj_t *s_about_obj = NULL;

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
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    const char *lines[10];
    char buf[10][48];
    snprintf(buf[0], sizeof(buf[0]), "系统: 小喵桌面");
    snprintf(buf[1], sizeof(buf[1]), "版本: %s", XIAOMIAO_VERSION);
    snprintf(buf[2], sizeof(buf[2]), "构建: %s", XIAOMIAO_BUILD);
    snprintf(buf[3], sizeof(buf[3]), "芯片: ESP32-WROVER-B");
    snprintf(buf[4], sizeof(buf[4]), "屏幕: ST7735 160x128");
    snprintf(buf[5], sizeof(buf[5]), "Python: %s",
             poincare_runtime_is_ready() ? "就绪" : "未初始化");
    snprintf(buf[6], sizeof(buf[6]), "字体: %s",
             lv_freetype_font_is_ready() ? "FreeType" : "内置");
    float vbat = drv_battery_get_voltage();
    if (vbat >= BAT_MIN_VALID_V) {
        int pct = drv_battery_get_percent(vbat);
        snprintf(buf[7], sizeof(buf[7]), "电池: %d%% (%.2fV)", pct, vbat);
    } else {
        snprintf(buf[7], sizeof(buf[7]), "电池: 未检测到");
    }
    snprintf(buf[8], sizeof(buf[8]), "内存: %d KB 空闲",
             heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
    extern uint8_t _rodata_start, _rodata_end, _data_start, _data_end, _bss_start, _bss_end;
    uint32_t flash_size = (uint32_t)&_rodata_end - (uint32_t)&_rodata_start
                        + (uint32_t)&_data_end - (uint32_t)&_data_start;
    snprintf(buf[9], sizeof(buf[9]), "固件: %lu KB", (unsigned long)(flash_size / 1024));
    lines[0] = buf[0]; lines[1] = buf[1]; lines[2] = buf[2];
    lines[3] = buf[3]; lines[4] = buf[4]; lines[5] = buf[5];
    lines[6] = buf[6]; lines[7] = buf[7]; lines[8] = buf[8]; lines[9] = buf[9];
    int item_h = (LCD_V_RES - 26 - DOCK_H) / 10;
    for (int i = 0; i < 10; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, lines[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
    }
    s_about_obj = list;
    ui_dock_create(scr, 1, 0);
}

static void about_destroy(void)
{
    ESP_LOGI(TAG, "About page destroy");
    s_about_obj = NULL;
}

static bool about_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
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