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

// 电话应用
static void phone_init(void *data);
static void phone_destroy(void);
static bool phone_on_key(int key);

// 游戏应用
static void games_init(void *data);
static void games_destroy(void);
static bool games_on_key(int key);

// 相机应用
static void camera_init(void *data);
static void camera_destroy(void);
static bool camera_on_key(int key);

// 音乐应用
static void music_init(void *data);
static void music_destroy(void);
static bool music_on_key(int key);

// 浏览器应用
static void browser_init(void *data);
static void browser_destroy(void);
static bool browser_on_key(int key);

// 笔记应用
static void notes_init(void *data);
static void notes_destroy(void);
static bool notes_on_key(int key);

// 关于应用
static void about_init(void *data);
static void about_destroy(void);
static bool about_on_key(int key);

/* ========== 页面回调定义 ========== */
static const page_callbacks_t s_settings_callbacks = {
    .init = settings_init,
    .activate = settings_activate,
    .destroy = settings_destroy,
    .on_key = settings_on_key,
};

static const page_callbacks_t s_phone_callbacks = {
    .init = phone_init,
    .destroy = phone_destroy,
    .on_key = phone_on_key,
};

static const page_callbacks_t s_games_callbacks = {
    .init = games_init,
    .destroy = games_destroy,
    .on_key = games_on_key,
};

static const page_callbacks_t s_camera_callbacks = {
    .init = camera_init,
    .destroy = camera_destroy,
    .on_key = camera_on_key,
};

static const page_callbacks_t s_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};

static const page_callbacks_t s_browser_callbacks = {
    .init = browser_init,
    .destroy = browser_destroy,
    .on_key = browser_on_key,
};

static const page_callbacks_t s_notes_callbacks = {
    .init = notes_init,
    .destroy = notes_destroy,
    .on_key = notes_on_key,
};

static const page_callbacks_t s_about_callbacks = {
    .init = about_init,
    .destroy = about_destroy,
    .on_key = about_on_key,
};

/* ========== 内置应用定义 ========== */
static const app_def_t s_builtin_app_defs[] = {
    {
        .name = "Settings",
        .icon_text = "S",
        .icon_color = 0x3B82F6,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,  // 由app_manager处理
    },
    {
        .name = "Phone",
        .icon_text = "P",
        .icon_color = 0x8B5CF6,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Games",
        .icon_text = "G",
        .icon_color = 0xF43F5E,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Camera",
        .icon_text = "C",
        .icon_color = 0xF59E0B,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Music",
        .icon_text = "M",
        .icon_color = 0x22C55E,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Browser",
        .icon_text = "B",
        .icon_color = 0x06B6D4,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Notes",
        .icon_text = "N",
        .icon_color = 0xEC4899,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "About",
        .icon_text = "?",
        .icon_color = 0x64748B,
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
    if (strcmp(app_name, "Settings") == 0) return &s_settings_callbacks;
    if (strcmp(app_name, "Phone") == 0) return &s_phone_callbacks;
    if (strcmp(app_name, "Games") == 0) return &s_games_callbacks;
    if (strcmp(app_name, "Camera") == 0) return &s_camera_callbacks;
    if (strcmp(app_name, "Music") == 0) return &s_music_callbacks;
    if (strcmp(app_name, "Browser") == 0) return &s_browser_callbacks;
    if (strcmp(app_name, "Notes") == 0) return &s_notes_callbacks;
    if (strcmp(app_name, "About") == 0) return &s_about_callbacks;
    return NULL;
}

/* ========== 设置应用实现 ========== */
#define SETTINGS_HDR_H  14
#define SETTINGS_ITEM_H  14

static const char *s_settings_items[] = {
    "Brightness",
    "Theme",
    "Sound",
    "WiFi",
    "Layout",
    "Save & Exit",
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
        "Brightness", "Theme", "Sound", "WiFi", "Layout", "Save & Exit"
    };
    char buf[40];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", items[0], st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", items[1],
                     st->theme == THEME_DARK ? "Dark" : "Light"); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %s", items[2], st->sound_on ? "On" : "Off"); break;
    case 3: snprintf(buf, sizeof(buf), "%s: %s", items[3], st->wifi_on ? "On" : "Off"); break;
    case 4: snprintf(buf, sizeof(buf), "%s: %d per page",
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

    // 标题
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_pos(hdr, 0, 14);
    lv_obj_set_size(hdr, LCD_H_RES, SETTINGS_HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ht = lv_label_create(hdr);
    lv_label_set_text(ht, "Settings");
    lv_obj_set_style_text_color(ht, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(ht, &lv_font_montserrat_12, 0);
    lv_obj_align(ht, LV_ALIGN_LEFT_MID, 4, 0);

    // 设置项列表
    s_settings_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_settings_list);
    lv_obj_set_pos(s_settings_list, 0, 14 + SETTINGS_HDR_H);
    lv_obj_set_size(s_settings_list, LCD_H_RES, LCD_V_RES - 14 - SETTINGS_HDR_H - 10);
    lv_obj_clear_flag(s_settings_list, LV_OBJ_FLAG_SCROLLABLE);

    int item_h = (LCD_V_RES - 14 - SETTINGS_HDR_H - 10) / SETTINGS_ITEM_COUNT;
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_settings_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * item_h);
        lv_obj_set_size(row, LCD_H_RES, item_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[i] = lbl;
        settings_refresh_label(i);
    }

    // 底部 Dock（仅显示）
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
        // 保存并退出
        sys_nvs_save_settings(st->brightness, st->sound_on,
                              (int)st->theme, st->wifi_on, st->layout);
        ui_stack_pop();
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

/* ========== 其他应用占位实现 ========== */
static void phone_init(void *data) { ESP_LOGI(TAG, "Phone init"); }
static void phone_destroy(void) { ESP_LOGI(TAG, "Phone destroy"); }
static bool phone_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void games_init(void *data) { ESP_LOGI(TAG, "Games init"); }
static void games_destroy(void) { ESP_LOGI(TAG, "Games destroy"); }
static bool games_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void camera_init(void *data) { ESP_LOGI(TAG, "Camera init"); }
static void camera_destroy(void) { ESP_LOGI(TAG, "Camera destroy"); }
static bool camera_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void music_init(void *data) { ESP_LOGI(TAG, "Music init"); }
static void music_destroy(void) { ESP_LOGI(TAG, "Music destroy"); }
static bool music_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void browser_init(void *data) { ESP_LOGI(TAG, "Browser init"); }
static void browser_destroy(void) { ESP_LOGI(TAG, "Browser destroy"); }
static bool browser_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void notes_init(void *data) { ESP_LOGI(TAG, "Notes init"); }
static void notes_destroy(void) { ESP_LOGI(TAG, "Notes destroy"); }
static bool notes_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}

static void about_init(void *data) { ESP_LOGI(TAG, "About init"); }
static void about_destroy(void) { ESP_LOGI(TAG, "About destroy"); }
static bool about_on_key(int key) {
    if (key == KEY_B) { ui_stack_pop(); return true; }
    return false;
}