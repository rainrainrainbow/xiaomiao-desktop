/**
 * @file app_builtin.h
 * @brief 内置应用注册中心 - 声明各独立应用的页面回调
 *
 * 架构说明：
 * 每个内置应用独立为一个 .c 文件，通过本头文件暴露其 page_callbacks_t。
 * app_builtin.c 作为注册中心，统一管理应用定义和回调查找。
 *
 * 参考 LiClock 的 App 架构设计：
 * - 每个 App 独立文件（src/apps/appSettings.cpp 等）
 * - App 继承 AppBase 基类，统一生命周期
 * - AppManager 栈式管理
 */

#ifndef APP_BUILTIN_H
#define APP_BUILTIN_H

#include "ui_framework.h"

/* ========== 各应用页面回调声明 ========== */

/**
 * 设置应用页面回调
 */
extern const page_callbacks_t g_settings_callbacks;

/**
 * 关于系统页面回调
 */
extern const page_callbacks_t g_about_callbacks;

/**
 * 应用管理页面回调
 */
extern const page_callbacks_t g_applist_callbacks;

/**
 * 积木编辑器页面回调
 */
extern const page_callbacks_t g_editor_callbacks;

/**
 * 商店页面回调
 */
extern const page_callbacks_t g_store_callbacks;

/**
 * 音乐页面回调
 */
extern const page_callbacks_t g_music_callbacks;

/**
 * 文件管理页面回调
 */
extern const page_callbacks_t g_filemgr_callbacks;

/**
 * MID蜂鸣器播放器页面回调
 */
extern const page_callbacks_t g_midplayer_callbacks;

/**
 * WiFi设置二级页面回调
 */
extern const page_callbacks_t g_wifi_settings_callbacks;

/**
 * 字体设置二级页面回调
 */
extern const page_callbacks_t g_font_settings_callbacks;

/**
 * 亮度设置二级页面回调
 */
extern const page_callbacks_t g_brightness_settings_callbacks;

/**
 * 音量设置二级页面回调
 */
extern const page_callbacks_t g_volume_settings_callbacks;

/**
 * 主题设置二级页面回调
 */
extern const page_callbacks_t g_theme_settings_callbacks;

/**
 * 布局设置二级页面回调
 */
extern const page_callbacks_t g_layout_settings_callbacks;

/**
 * 屏幕超时设置二级页面回调
 */
extern const page_callbacks_t g_sleep_settings_callbacks;

/**
 * 日期时间设置二级页面回调
 */
extern const page_callbacks_t g_datetime_settings_callbacks;

#endif /* APP_BUILTIN_H */