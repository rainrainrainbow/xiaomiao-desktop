/**
 * @file app_builtin.c
 * @brief 内置应用注册中心
 *
 * 架构说明：
 * 每个内置应用独立为一个 .c 文件，通过 app_builtin.h 暴露 page_callbacks_t。
 * 本文件作为注册中心，统一管理应用定义数组、注册函数和回调查找。
 *
 * 参考 LiClock 的 App 架构设计：
 * - 每个 App 独立文件（src/apps/appSettings.cpp 等）
 * - App 继承 AppBase 基类，统一生命周期
 * - AppManager 栈式管理
 */

#include "app_builtin.h"
#include "app_manager.h"
#include "app_micropython.h"
#include "ui_framework.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_BUILTIN";

/* ========== 内置应用定义 ========== */
/*
 * 名称用中文（LVGL 内置图形符号 + CJK 中文字体）
 * 图标用 LVGL 内置符号（LV_SYMBOL_* 支持，无乱码）
 * 模拟器 6 个应用/屏
 */
static const app_def_t s_builtin_app_defs[] = {
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
        .name = "音乐",
        .icon_text = LV_SYMBOL_AUDIO,
        .icon_color = 0x8B5CF6,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "Python",
        .icon_text = LV_SYMBOL_COPY,
        .icon_color = 0x3B82F6,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "文件",
        .icon_text = LV_SYMBOL_FILE,
        .icon_color = 0x10B981,
        .type = APP_TYPE_BUILTIN,
        .launch_cb = NULL,
    },
    {
        .name = "MID播放",
        .icon_text = LV_SYMBOL_AUDIO,
        .icon_color = 0xF59E0B,
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
    if (strcmp(app_name, "设置") == 0) return &g_settings_callbacks;
    if (strcmp(app_name, "应用") == 0) return &g_applist_callbacks;
    if (strcmp(app_name, "积木") == 0) return &g_editor_callbacks;
    if (strcmp(app_name, "商店") == 0) return &g_store_callbacks;
    if (strcmp(app_name, "音乐") == 0) return &g_music_callbacks;
    if (strcmp(app_name, "Python") == 0) return app_micropython_get_callbacks();
    if (strcmp(app_name, "文件") == 0) return &g_filemgr_callbacks;
    if (strcmp(app_name, "MID播放") == 0) return &g_midplayer_callbacks;
    if (strcmp(app_name, "WiFi设置") == 0) return &g_wifi_settings_callbacks;
    if (strcmp(app_name, "字体设置") == 0) return &g_font_settings_callbacks;
    if (strcmp(app_name, "亮度设置") == 0) return &g_brightness_settings_callbacks;
    if (strcmp(app_name, "音量设置") == 0) return &g_volume_settings_callbacks;
    if (strcmp(app_name, "主题设置") == 0) return &g_theme_settings_callbacks;
    if (strcmp(app_name, "布局设置") == 0) return &g_layout_settings_callbacks;
    if (strcmp(app_name, "屏幕超时") == 0) return &g_sleep_settings_callbacks;
    if (strcmp(app_name, "日期时间") == 0) return &g_datetime_settings_callbacks;
    return NULL;
}

/* 桌面"应用"图标点击时，直接进入设置中的应用管理二级页面 */
void app_launch_app_manager(void)
{
    // 先推入设置页面，再推入应用管理页面
    // 这样用户按 B 会先回到设置，再回到桌面
    const page_callbacks_t *settings_cbs = app_builtin_get_callbacks("设置");
    if (settings_cbs) {
        ui_stack_push(PAGE_APP_PLACEHOLDER, settings_cbs, NULL);
    }
    ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
}