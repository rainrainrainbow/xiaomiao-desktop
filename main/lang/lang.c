/**
 * @file lang.c
 * @brief 多语言支持实现
 */
#include "lang.h"
#include "esp_log.h"

static const char *TAG = "LANG";

/* ========== 中文翻译表 ========== */
static const char *s_zh[STR_COUNT] = {
    [STR_WIFI]            = "WiFi",
    [STR_WIFI_ON]         = "开",
    [STR_WIFI_OFF]        = "关",
    [STR_WIFI_SCANNING]   = "正在扫描...",
    [STR_WIFI_CONNECTING] = "正在连接...",
    [STR_WIFI_CONNECTED]  = "已连接",
    [STR_WIFI_CLOSED]     = "WiFi已关闭",
    [STR_WIFI_SCANNED]    = "已扫描",
    [STR_BRIGHTNESS]      = "亮度",
    [STR_VOLUME]          = "音量",
    [STR_THEME]           = "主题",
    [STR_THEME_DARK]      = "深色",
    [STR_THEME_LIGHT]     = "浅色",
    [STR_LAYOUT]          = "布局",
    [STR_LAYOUT_3COL]     = "3列",
    [STR_LAYOUT_2COL]     = "2列",
    [STR_FONT]            = "字体",
    [STR_SOUND]           = "声音",
    [STR_SOUND_ON]        = "开",
    [STR_SOUND_OFF]       = "关",
    [STR_SLEEP_TIMEOUT]   = "屏幕超时",
    [STR_DATE_TIME]       = "日期时间",
    [STR_APP_MANAGER]     = "应用管理",
    [STR_ABOUT]           = "关于系统",
    [STR_RESET_DEFAULT]   = "恢复默认",
    [STR_SAVE_EXIT]       = "保存并退出",
    [STR_RETURN_LOADER]   = "返回Loader",
    [STR_SETTINGS]        = "设置",
    [STR_DESKTOP]         = "桌面",
    [STR_RECENTS]         = "最近任务",
    [STR_RECENTS_EMPTY]   = "暂无最近任务",
    [STR_CURRENT]         = "当前",
    [STR_BACKGROUND]      = "后台",
    [STR_BACK]            = "返回",
    [STR_SYSTEM]          = "系统",
    [STR_VERSION]         = "版本",
    [STR_BUILD]           = "构建",
    [STR_CHIP]            = "芯片",
    [STR_SCREEN]          = "屏幕",
    [STR_PYTHON]          = "Python",
    [STR_FONT_ENGINE]     = "字体引擎",
    [STR_BATTERY]         = "电池",
    [STR_MEMORY]          = "内存",
    [STR_FIRMWARE]        = "固件",
    [STR_CPU_FREQ]        = "CPU频率",
    [STR_PSRAM]           = "PSRAM",
    [STR_FLASH]           = "Flash",
    [STR_UPTIME]          = "运行时间",
    [STR_SDK_VERSION]     = "SDK版本",
    [STR_LVGL_VERSION]    = "LVGL版本",
    [STR_RETRO_CORE]      = "存储分区",
    [STR_LANGUAGE]        = "语言",
    [STR_APP_SETTINGS]    = "设置",
    [STR_APP_SNAKE]       = "贪吃蛇",
    [STR_APP_MUSIC]       = "音乐",
    [STR_APP_EDITOR]      = "积木",
    [STR_APP_STORE]       = "商店",
    [STR_APP_FILES]       = "文件",
    [STR_APP_MID]         = "MID播放器",
    [STR_APP_PYTHON]      = "Python",
    [STR_APP_APPS]        = "应用",
};

/* ========== 英文翻译表 ========== */
static const char *s_en[STR_COUNT] = {
    [STR_WIFI]            = "WiFi",
    [STR_WIFI_ON]         = "On",
    [STR_WIFI_OFF]        = "Off",
    [STR_WIFI_SCANNING]   = "Scanning...",
    [STR_WIFI_CONNECTING] = "Connecting...",
    [STR_WIFI_CONNECTED]  = "Connected",
    [STR_WIFI_CLOSED]     = "WiFi Off",
    [STR_WIFI_SCANNED]    = "Scanned",
    [STR_BRIGHTNESS]      = "Brightness",
    [STR_VOLUME]          = "Volume",
    [STR_THEME]           = "Theme",
    [STR_THEME_DARK]      = "Dark",
    [STR_THEME_LIGHT]     = "Light",
    [STR_LAYOUT]          = "Layout",
    [STR_LAYOUT_3COL]     = "3 Col",
    [STR_LAYOUT_2COL]     = "2 Col",
    [STR_FONT]            = "Font",
    [STR_SOUND]           = "Sound",
    [STR_SOUND_ON]        = "On",
    [STR_SOUND_OFF]       = "Off",
    [STR_SLEEP_TIMEOUT]   = "Sleep",
    [STR_DATE_TIME]       = "Date/Time",
    [STR_APP_MANAGER]     = "App Manager",
    [STR_ABOUT]           = "About",
    [STR_RESET_DEFAULT]   = "Reset Defaults",
    [STR_SAVE_EXIT]       = "Save & Exit",
    [STR_RETURN_LOADER]   = "Return to Loader",
    [STR_SETTINGS]        = "Settings",
    [STR_DESKTOP]         = "Desktop",
    [STR_RECENTS]         = "Recents",
    [STR_RECENTS_EMPTY]   = "No recent apps",
    [STR_CURRENT]         = "Current",
    [STR_BACKGROUND]      = "Background",
    [STR_BACK]            = "Back",
    [STR_SYSTEM]          = "System",
    [STR_VERSION]         = "Version",
    [STR_BUILD]           = "Build",
    [STR_CHIP]            = "Chip",
    [STR_SCREEN]          = "Display",
    [STR_PYTHON]          = "Python",
    [STR_FONT_ENGINE]     = "Font Engine",
    [STR_BATTERY]         = "Battery",
    [STR_MEMORY]          = "Memory",
    [STR_FIRMWARE]        = "Firmware",
    [STR_CPU_FREQ]        = "CPU Freq",
    [STR_PSRAM]           = "PSRAM",
    [STR_FLASH]           = "Flash",
    [STR_UPTIME]          = "Uptime",
    [STR_SDK_VERSION]     = "SDK Ver",
    [STR_LVGL_VERSION]    = "LVGL Ver",
    [STR_RETRO_CORE]      = "Storage",
    [STR_LANGUAGE]        = "Language",
    [STR_APP_SETTINGS]    = "Settings",
    [STR_APP_SNAKE]       = "Snake",
    [STR_APP_MUSIC]       = "Music",
    [STR_APP_EDITOR]      = "Blocks",
    [STR_APP_STORE]       = "Store",
    [STR_APP_FILES]       = "Files",
    [STR_APP_MID]         = "MID Player",
    [STR_APP_PYTHON]      = "Python",
    [STR_APP_APPS]        = "Apps",
};

static lang_id_t s_current_lang = LANG_ZH;

void lang_set(lang_id_t lang)
{
    if (lang >= LANG_MAX) return;
    s_current_lang = lang;
    ESP_LOGI(TAG, "Language set to %s", lang == LANG_ZH ? "中文" : "English");
}

lang_id_t lang_get_current(void)
{
    return s_current_lang;
}

const char* lang_get(str_id_t id)
{
    if (id >= STR_COUNT) return "";
    const char *str = NULL;
    switch (s_current_lang) {
    case LANG_EN:
        str = s_en[id];
        break;
    case LANG_ZH:
    default:
        str = s_zh[id];
        break;
    }
    return str ? str : "";
}