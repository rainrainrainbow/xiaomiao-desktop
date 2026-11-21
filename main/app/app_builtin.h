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
#include <stdint.h>
#include <stdbool.h>

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

/* MID蜂鸣器播放器页面回调 - 已删除 */

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

/**
 * 音频输出设置二级页面回调
 */
extern const page_callbacks_t g_audio_settings_callbacks;

/**
 * 蓝牙设置二级页面回调
 */
extern const page_callbacks_t g_bt_settings_callbacks;

/**
 * 字库选择设置二级页面回调
 */
extern const page_callbacks_t g_font_source_settings_callbacks;

/**
 * 语言设置二级页面回调
 */
extern const page_callbacks_t g_language_settings_callbacks;

/**
 * OTA更新页面回调
 */
extern const page_callbacks_t g_ota_callbacks;

/**
 * 恢复模式页面回调（类安卓 Recovery 的系统维护菜单）
 */
extern const page_callbacks_t g_recovery_callbacks;

/**
 * 灯效应用页面回调
 */
extern const page_callbacks_t g_led_callbacks;
/**
 * Shell 命令终端页面回调（原 Python 应用改造）
 */
extern const page_callbacks_t g_shell_callbacks;
/**
 * 根据内置应用内部名称获取本地化显示名
 * @param internal_name 内部名称（中文，如"设置"、"商店"）
 * @return 本地化后的显示名
 */
const char* app_builtin_get_display_name(const char *internal_name);

/**
 * 音乐应用只读诊断状态（供自动化测试通过 XTC cmd=15 查询）
 *
 * 注意：这是诊断接口，不替代 UI 操作；字段直接来自 music app 的运行时状态。
 */
typedef struct {
    char dir[128];          /* 当前扫描目录（/flash/music 或 /sdcard/music） */
    char current_file[128]; /* 当前选中/播放的文件名 */
    int count;              /* 扫描到的音频文件数 */
    int selected;           /* 列表选中索引 */
    int playing_idx;        /* 正在播放的索引，-1=未播放 */
    bool is_playing;
    bool is_paused;
    int src;                /* 0=unknown, 1=WAV, 2=MP3 */
    int sample_rate;
    int channels;
    uint32_t bytes_read;    /* 解码器已输出 PCM 字节数 */
    uint32_t position_ms;   /* 解码器报告的播放位置 */
} app_music_status_t;

/** 读取音乐应用当前状态；未启动时 count=0、is_playing=false */
void app_music_get_status(app_music_status_t *out);

/**
 * 音乐应用诊断控制（仅供 XTC cmd=16 使用）：
 *   op=1 重新扫描音乐目录；op=2 播放指定索引；op=3 停止播放
 * 请求在内部 RAM 栈的独立任务中执行，避免从 UART 任务直接操作解码器/UI。
 */
void app_music_debug_control(int op, int idx);

#endif /* APP_BUILTIN_H */