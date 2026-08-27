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
    STR_APP_SHELL,           // Shell 命令终端
    STR_APP_APPS,
    STR_APP_LED,
    // 应用类型标签
    STR_APP_TYPE_BUILTIN,
    STR_APP_TYPE_PYTHON,
    // 设置项名称（二级页面标题）
    STR_AUDIO_OUTPUT,
    // 布局设置
    STR_LAYOUT_3COL_DESC,
    STR_LAYOUT_2COL_DESC,
    STR_LAYOUT_HINT,
    STR_CURRENT_VALUE,
    // 字体设置
    STR_FONT_SIZE_SMALL,
    STR_FONT_SIZE_MEDIUM,
    STR_FONT_SIZE_LARGE,
    STR_FONT_SIZE_XLARGE,
    STR_FONT_PREVIEW,
    // 屏幕超时
    STR_SLEEP_NEVER,
    STR_SLEEP_30S,
    STR_SLEEP_60S,
    STR_SLEEP_2M,
    STR_SLEEP_5M,
    // 音频设置
    STR_AUDIO_MODE,
    STR_AUDIO_MODE_AUTO,
    STR_AUDIO_MODE_MANUAL,
    STR_AUDIO_UNAVAILABLE,
    STR_AUDIO_HINT,
    // 亮度/音量提示
    STR_BRIGHTNESS_HINT,
    STR_VOLUME_HINT,
    // 音乐提示
    STR_MUSIC_PLAY_HINT,
    STR_MUSIC_HINT,
    // 音乐播放器三页面
    STR_MUSIC_LIST,          // 播放列表
    STR_MUSIC_SETTINGS,      // 音乐设置
    STR_MUSIC_SPECTRUM,      // 频谱
    STR_MUSIC_LYRICS,        // 歌词
    STR_MUSIC_LRC_OFF,       // 歌词:关
    STR_MUSIC_LRC_ON,        // 歌词:开
    STR_MUSIC_LOOP_MODE,     // 循环模式
    STR_MUSIC_LOOP_SINGLE,   // 单曲循环
    STR_MUSIC_LOOP_LIST,     // 列表循环
    STR_MUSIC_LOOP_RANDOM,   // 随机播放
    STR_MUSIC_PREV,          // 上一首
    STR_MUSIC_NEXT,          // 下一首
    STR_MUSIC_PLAY,          // 播放
    STR_MUSIC_PAUSE,         // 暂停
    STR_MUSIC_STOP,          // 停止
    STR_MUSIC_PLAYING,       // 正在播放
    STR_MUSIC_NO_FILE,       // 无音频文件
    STR_MUSIC_SPECTRUM_OFF,  // 频谱:关
    STR_MUSIC_SPECTRUM_ON,   // 频谱:开
    STR_MUSIC_SETTINGS_HINT, // 设置页操作提示
    STR_MUSIC_PLAY_HINT2,    // 播放页操作提示
    // 文件管理器
    STR_FILE_EMPTY_DIR,
    STR_FILE_UNSUPPORTED,
    STR_FILE_MID_REMOVED,
    STR_FILE_AUDIO_NA,
    // 商店
    STR_STORE_INSTALLED,
    STR_STORE_NOT_INSTALLED,
    STR_STORE_EMPTY,
    STR_STORE_INSTALL,
    STR_STORE_UNINSTALL,
    // MicroPython
    STR_MP_LOADING,
    STR_MP_OK,
    STR_MP_FAIL,
    STR_MP_NO_ENTRY,
    STR_MP_DONE,
    STR_MP_EXEC_FAIL,
    STR_MP_NO_SD_CARD,
    // 安装状态
    STR_INSTALL_OK,
    STR_INSTALL_BLOCKED,
    STR_INSTALL_UNTRUSTED,
    STR_INSTALL_UNKNOWN,
    // 日期时间
    STR_DATETIME_SYNC,
    STR_DATETIME_SYNCING,
    STR_DATETIME_SYNC_OK,
    STR_DATETIME_SYNC_FAIL,
    STR_DATETIME_SYNC_HINT,
    // WiFi
    STR_WIFI_MODE_STA,
    STR_WIFI_MODE_AP,
    STR_WIFI_AP_RUNNING,
    STR_WIFI_CONNECTED_PREFIX,
    STR_WIFI_SCANNED_PREFIX,
    STR_WIFI_NAME,
    STR_WIFI_PASSWORD,
    STR_WIFI_CHANNEL,
    STR_WIFI_SCAN_HINT,
    STR_IRAM,
    STR_LANGUAGE_ZH,
    STR_LANGUAGE_EN,
    // OTA
    STR_OTA_IDLE,
    STR_OTA_CHECKING,
    STR_OTA_DOWNLOADING,
    STR_OTA_READY,
    STR_OTA_ERROR,
    STR_OTA_LATEST_VERSION,
    STR_OTA_REBOOT_HINT,
    STR_OTA_RETRY_HINT,
    // 灯效
    STR_LED_EFFECT_OFF,
    STR_LED_EFFECT_STATIC,
    STR_LED_EFFECT_BREATH,
    STR_LED_EFFECT_RAINBOW,
    STR_LED_EFFECT_FLOW,
    STR_LED_EFFECT_BLINK,
    STR_LED_COLOR_RED,
    STR_LED_COLOR_ORANGE,
    STR_LED_COLOR_YELLOW,
    STR_LED_COLOR_GREEN,
    STR_LED_COLOR_CYAN,
    STR_LED_COLOR_BLUE,
    STR_LED_COLOR_PURPLE,
    STR_LED_COLOR_PINK,
    STR_LED_COLOR_WHITE,
    STR_LED_COLOR_SEP,
    STR_LED_BRIGHTNESS,
    // Shell 命令终端
    STR_SHELL_WELCOME,       // Shell 欢迎信息
    STR_SHELL_INPUT,         // 输入提示
    STR_SHELL_HELP,          // 帮助
    STR_SHELL_UNKNOWN,       // 未知命令
    STR_SHELL_SCROLL,        // 滚动提示
    // 设置-关于系统：状态/字体引擎/内存信息
    STR_STATUS_READY,        // Python 就绪
    STR_STATUS_NOT_READY,    // Python 未就绪
    STR_FONT_ENGINE_FREETYPE,// 字体引擎: FreeType
    STR_FONT_ENGINE_BUILTIN, // 字体引擎: 内置
    STR_MEM_MIN_FREE,        // 内存最低空闲
    STR_MEM_MAX_BLOCK,       // 内存最大连续块
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