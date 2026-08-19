/**
 * @file lang.h
 * @brief 多语言支持框架
 *
 * 支持中英文切换，所有UI字符串通过 lang_get() 获取。
 * 新增语言只需添加 lang_zh.h / lang_en.h 翻译表。
 */
#ifndef LANG_H
#define LANG_H

#include <string.h>
#include <stdbool.h>

/* ========== 语言ID ========== */
typedef enum {
    LANG_ZH = 0,  // 中文
    LANG_EN,      // 英文
    LANG_MAX
} lang_id_t;

/* ========== 字符串ID ========== */
typedef enum {
    STR_WIFI = 0,
    STR_WIFI_ON,
    STR_WIFI_OFF,
    STR_WIFI_SCANNING,
    STR_WIFI_CONNECTING,
    STR_WIFI_CONNECTED,
    STR_WIFI_CLOSED,
    STR_WIFI_SCANNED,
    STR_BRIGHTNESS,
    STR_VOLUME,
    STR_THEME,
    STR_THEME_DARK,
    STR_THEME_LIGHT,
    STR_LAYOUT,
    STR_LAYOUT_3COL,
    STR_LAYOUT_2COL,
    STR_FONT,
    STR_SOUND,
    STR_SOUND_ON,
    STR_SOUND_OFF,
    STR_SLEEP_TIMEOUT,
    STR_DATE_TIME,
    STR_APP_MANAGER,
    STR_ABOUT,
    STR_RESET_DEFAULT,
    STR_SAVE_EXIT,
    STR_RETURN_LOADER,
    STR_SETTINGS,
    STR_DESKTOP,
    STR_RECENTS,
    STR_RECENTS_EMPTY,
    STR_CURRENT,
    STR_BACKGROUND,
    STR_BACK,
    STR_SYSTEM,
    STR_VERSION,
    STR_BUILD,
    STR_CHIP,
    STR_SCREEN,
    STR_PYTHON,
    STR_FONT_ENGINE,
    STR_BATTERY,
    STR_MEMORY,
    STR_FIRMWARE,
    STR_CPU_FREQ,
    STR_PSRAM,
    STR_FLASH,
    STR_UPTIME,
    STR_SDK_VERSION,
    STR_LVGL_VERSION,
    STR_RETRO_CORE,
    STR_LANGUAGE,
    STR_FONT_SOURCE,
    STR_FONT_SOURCE_FREETYPE,
    STR_FONT_SOURCE_BUILTIN,
    // 应用名
    STR_APP_SETTINGS,
    STR_APP_MUSIC,
    STR_APP_STORE,
    STR_APP_FILES,
    STR_APP_MID,
    STR_APP_PYTHON,
    STR_APP_APPS,
    // 应用类型标签
    STR_APP_TYPE_BUILTIN,
    STR_APP_TYPE_PYTHON,
    // 总数
    STR_COUNT
} str_id_t;

/**
 * 设置当前语言
 */
void lang_set(lang_id_t lang);

/**
 * 获取当前语言ID
 */
lang_id_t lang_get_current(void);

/**
 * 获取字符串翻译
 * @param id 字符串ID
 * @return 翻译后的字符串（UTF-8）
 */
const char* lang_get(str_id_t id);

#endif /* LANG_H */