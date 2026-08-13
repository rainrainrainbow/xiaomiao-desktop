/**
 * @file app_builtin.c
 * @brief 内置应用集合 - 设置、电话、游戏、相机、音乐、浏览器、笔记、关于
 */

#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "system/sys_nvs.h"
#include "driver/drv_backlight.h"
#include <string.h>

static const char *TAG = "APP_BUILTIN";

/* ========== 应用页面回调实现 ========== */

// 设置应用
static void settings_init(void *data);
static void settings_activate(void);
static void settings_destroy(void);
static bool settings_on_key(int key);

// 应用列表
static void applist_init(void *data);
static void applist_destroy(void);
static bool applist_on_key(int key);

// 积木编辑器
static void editor_init(void *data);
static void editor_destroy(void);
static bool editor_on_key(int key);

// 商店
static void store_init(void *data);
static void store_destroy(void);
static bool store_on_key(int key);

// 贪吃蛇
static void snake_init(void *data);
static void snake_destroy(void);
static bool snake_on_key(int key);

// 音乐
static void music_init(void *data);
static void music_destroy(void);
static bool music_on_key(int key);

/* ========== 页面回调定义 ========== */
static const page_callbacks_t s_settings_callbacks = {
    .init = settings_init,
    .activate = settings_activate,
    .destroy = settings_destroy,
    .on_key = settings_on_key,
};

static const page_callbacks_t s_applist_callbacks = {
    .init = applist_init,
    .destroy = applist_destroy,
    .on_key = applist_on_key,
};

static const page_callbacks_t s_editor_callbacks = {
    .init = editor_init,
    .destroy = editor_destroy,
    .on_key = editor_on_key,
};

static const page_callbacks_t s_store_callbacks = {
    .init = store_init,
    .destroy = store_destroy,
    .on_key = store_on_key,
};

static const page_callbacks_t s_snake_callbacks = {
    .init = snake_init,
    .destroy = snake_destroy,
    .on_key = snake_on_key,
};

static const page_callbacks_t s_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};

/* ========== 内置应用定义 ==========
 * 名称用中文（LVGL 内置图形符号 + CJK 中文字体）
 * 图标用 LVGL 内置符号（LV_SYMBOL_* 支持，无乱码）
 * 模拟器 6 个应用/屏
 */
static const app_def_t s_builtin_app_defs[] = {
    {
        .name = "应用",
        .icon_text = LV_SYMBOL_LIST,
        .icon_color = 0xF6D34A,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "设置",
        .icon_text = LV_SYMBOL_SETTINGS,
        .icon_color = 0x5C4220,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "积木",
        .icon_text = LV_SYMBOL_EDIT,
        .icon_color = 0x2DD466,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "商店",
        .icon_text = LV_SYMBOL_DOWNLOAD,
        .icon_color = 0xE64B3C,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "贪吃蛇",
        .icon_text = LV_SYMBOL_PLAY,
        .icon_color = 0x22C55E,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "音乐",
        .icon_text = LV_SYMBOL_AUDIO,
        .icon_color = 0x8B5CF6,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
};

#define BUILTIN_APP_COUNT (sizeof(s_builtin_app_defs) / sizeof(s_builtin_app_defs[0]))

/* ========== 注册所有内置应用 ========== */
void app_builtin_register_all(void)
{
    for (int i = 0; i < BUILTIN_APP_COUNT; i++) {
        app_register_builtin(&s_builtin_app_defs[i]);
    }
    ESP_LOGI(TAG, "Registered %d builtin apps", BUILTIN_APP_COUNT);
}

/* ========== 获取应用页面回调 ========== */
const page_callbacks_t* app_builtin_get_callbacks(const char *app_name)
{
    if (strcmp(app_name, "设置") == 0) return &s_settings_callbacks;
    if (strcmp(app_name, "应用") == 0) return &s_applist_callbacks;
    if (strcmp(app_name, "积木") == 0) return &s_editor_callbacks;
    if (strcmp(app_name, "商店") == 0) return &s_store_callbacks;
    if (strcmp(app_name, "贪吃蛇") == 0) return &s_snake_callbacks;
    if (strcmp(app_name, "音乐") == 0) return &s_music_callbacks;
    return NULL;
}

/* ========== 设置应用实现 ========== */
#define SETTINGS_HDR_H  12
#define SETTINGS_ITEM_H  14

static const char *s_settings_items[] = {
    "亮度",
    "主题",
    "声音",
    "WiFi",
    "布局",
    "保存并退出",
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[6] = {0};
static int s_settings_sel = 0;

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    const char *items[] = {
        "亮度", "主题", "声音", "WiFi", "布局", "保存并退出"
    };
    char buf[40];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", items[0], st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", items[1],
                     st->theme == THEME_DARK ? "深色" : "浅色"); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %s", items[2], st->sound_on ? "开" : "关"); break;
    case 3: snprintf(buf, sizeof(buf), "%s: %s", items[3], st->wifi_on ? "开" : "关"); break;
    case 4: snprintf(buf, sizeof(buf), "%s: %d 每页",
                     items[4], st->layout == 0 ? 4 : 2); break;
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

    // 清空当前屏
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 状态栏
    ui_statusbar_create(scr);

    // 标题栏（模拟器 titlebar 风格，使用 CJK 14px 字体显示中文）
    ui_titlebar_create(scr, 14, "设置");

    // 设置项列表
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
        // 设置项含中文，使用 CJK 字体
        LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
        lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[i] = lbl;
        settings_refresh_label(i);
    }

    // 底部 Dock
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
    for (int i = 0; i < 6; i++) s_settings_labels[i] = NULL;
    s_settings_list = NULL;
}

static bool settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();

    if (key == KEY_B) {
        // 保存并退出（仅当栈深>1时，避免弹出桌面导致崩溃）
        if (ui_stack_depth() > 1) {
            sys_nvs_save_settings(st->brightness, st->sound_on,
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
        case 0: // 亮度
            st->brightness += delta * 10;
            if (st->brightness < 10) st->brightness = 10;
            if (st->brightness > 100) st->brightness = 100;
            drv_backlight_set_brightness(st->brightness);
            break;
        case 1: // 主题
            st->theme = (st->theme == THEME_DARK) ? THEME_LIGHT : THEME_DARK;
            ui_theme_set(st->theme);
            break;
        case 2: // 声音
            st->sound_on = !st->sound_on;
            break;
        case 3: // WiFi
            st->wifi_on = !st->wifi_on;
            break;
        case 4: // 布局
            st->layout = (st->layout == 0) ? 1 : 0;
            break;
        case 5: // Save & Exit
            sys_nvs_save_settings(st->brightness, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout);
            ui_stack_pop();
            return true;
        }
        settings_refresh_label(s_settings_sel);
        return true;
    }

    return false;
}

/* ========== 应用列表页 ========== */
static lv_obj_t *s_applist_obj = NULL;
static int s_applist_sel = 0;

static void applist_init(void *data)
{
    ESP_LOGI(TAG, "App list init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 状态栏
    ui_statusbar_create(scr);
    // 标题栏
    ui_titlebar_create(scr, 14, "全部应用");

    int builtin_count;
    const app_def_t *apps = app_manager_get_builtin(&builtin_count);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    int item_h = 14;
    for (int i = 0; i < builtin_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s", apps[i].icon_text, apps[i].name);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        // 应用名为中文，使用 CJK 字体
        LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
        lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }

    s_applist_obj = list;
    ui_dock_create(scr, 1, 0);
}

static void applist_destroy(void)
{
    ESP_LOGI(TAG, "App list destroy");
    s_applist_obj = NULL;
}

static bool applist_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    int builtin_count;
    const app_def_t *apps = app_manager_get_builtin(&builtin_count);

    const theme_colors_t *colors = ui_theme_colors();
    // 取消选中
    lv_obj_t *list = s_applist_obj;
    if (!list) return false;
    lv_obj_t *old_row = lv_obj_get_child(list, s_applist_sel);
    if (old_row) lv_obj_set_style_bg_opa(old_row, LV_OPA_TRANSP, 0);

    if (key == KEY_UP) s_applist_sel = (s_applist_sel - 1 + builtin_count) % builtin_count;
    if (key == KEY_DOWN) s_applist_sel = (s_applist_sel + 1) % builtin_count;

    // 选中
    lv_obj_t *new_row = lv_obj_get_child(list, s_applist_sel);
    if (new_row) {
        lv_obj_set_style_bg_color(new_row, lv_color_hex(0x5C4220), 0);
        lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, 0);
    }

    if (key == KEY_A) {
        if (s_applist_sel < builtin_count) {
            app_manager_launch(&apps[s_applist_sel]);
            return true;
        }
    }
    return true;
}

/* ========== 积木编辑器 ========== */
static lv_obj_t *s_editor_obj = NULL;
static int s_editor_pane = 0;  // 0=积木库, 1=程序区

static void editor_init(void *data)
{
    ESP_LOGI(TAG, "Editor init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "积木编辑器");

    // 左右分栏（模拟器 .ed-split 风格）
    lv_obj_t *split = lv_obj_create(scr);
    lv_obj_remove_style_all(split);
    lv_obj_set_pos(split, 0, 26);
    lv_obj_set_size(split, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(split, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(split, LV_FLEX_ALIGN_SPACE_EVENLY, 
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 积木库面板
    lv_obj_t *pane_l = lv_obj_create(split);
    lv_obj_remove_style_all(pane_l);
    lv_obj_set_size(pane_l, 76, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(pane_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane_l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pane_l, 1, 0);
    lv_obj_set_style_pad_row(pane_l, 1, 0);
    lv_obj_set_style_bg_color(pane_l, lv_color_hex(0xFFF3B0), 0); // cream bg
    lv_obj_set_style_bg_opa(pane_l, LV_OPA_COVER, 0);

    lv_obj_t *hdr_l = lv_label_create(pane_l);
    lv_label_set_text(hdr_l, "积木库");
    lv_obj_set_style_text_color(hdr_l, lv_color_hex(0x5C4220), 0);
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(hdr_l, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_set_style_text_align(hdr_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hdr_l, 76);

    // 程序区面板
    lv_obj_t *pane_r = lv_obj_create(split);
    lv_obj_remove_style_all(pane_r);
    lv_obj_set_size(pane_r, 76, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(pane_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane_r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pane_r, 1, 0);
    lv_obj_set_style_pad_row(pane_r, 1, 0);

    lv_obj_t *hdr_r = lv_label_create(pane_r);
    lv_label_set_text(hdr_r, "程序区");
    lv_obj_set_style_text_color(hdr_r, lv_color_hex(0x5C4220), 0);
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(hdr_r, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_set_style_text_align(hdr_r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hdr_r, 76);

    s_editor_obj = split;
    ui_dock_create(scr, 1, 0);
}

static void editor_destroy(void)
{
    ESP_LOGI(TAG, "Editor destroy");
    s_editor_obj = NULL;
}

static bool editor_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    if (key == KEY_LEFT) { s_editor_pane = 0; return true; }
    if (key == KEY_RIGHT) { s_editor_pane = 1; return true; }
    return true;
}

/* ========== 商店 ========== */
static lv_obj_t *s_store_obj = NULL;
static int s_store_sel = 0;

static void store_init(void *data)
{
    ESP_LOGI(TAG, "Store init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "应用商店");

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    s_store_obj = list;
    ui_dock_create(scr, 1, 0);
}

static void store_destroy(void)
{
    ESP_LOGI(TAG, "Store destroy");
    s_store_obj = NULL;
}

static bool store_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return true;
}

/* ========== 贪吃蛇 ========== */
static void snake_init(void *data)
{
    ESP_LOGI(TAG, "Snake init - placeholder");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF6D34A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "贪吃蛇");
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "敬请期待");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x1B1713), 0);
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 6);
}

static void snake_destroy(void) { ESP_LOGI(TAG, "Snake destroy"); }
static bool snake_on_key(int key) {
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return true;
}

/* ========== 音乐 ========== */
static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music init - placeholder");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF6D34A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "音乐");
    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "敬请期待");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x1B1713), 0);
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 6);
}

static void music_destroy(void) { ESP_LOGI(TAG, "Music destroy"); }
static bool music_on_key(int key) {
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return true;
}