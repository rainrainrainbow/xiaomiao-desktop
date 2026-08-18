/**
 * @file sys_nvs.h
 * @brief NVS存储 - 持久化保存用户设置
 */

#ifndef SYS_NVS_H
#define SYS_NVS_H

#include <stdbool.h>

/* ========== NVS存储键名 ========== */
#define NVS_NAMESPACE       "xiaomiao"
#define NVS_KEY_BRIGHTNESS  "brightness"
#define NVS_KEY_VOLUME      "volume"
#define NVS_KEY_SOUND       "sound"
#define NVS_KEY_THEME       "theme"
#define NVS_KEY_WIFI        "wifi"
#define NVS_KEY_LAYOUT      "layout"
#define NVS_KEY_FONT_SIZE   "font_size"
#define NVS_KEY_SLEEP       "sleep"
#define NVS_KEY_FIRST_RUN   "first_run"  // 首次运行标志
#define NVS_KEY_AUDIO_OUT   "audio_out"  // 音频输出设备类型
#define NVS_KEY_AUDIO_AUTO  "audio_auto" // 音频自动选择模式

/* ========== NVS存储接口 ========== */

/**
 * 初始化NVS存储
 * @return 0成功，其他失败
 */
int sys_nvs_init(void);

/**
 * 保存设置到NVS
 * @param brightness 亮度 (10-100)
 * @param volume 音量 (0-100)
 * @param sound_on 声音开关
 * @param theme 主题 (0=Dark, 1=Light)
 * @param wifi_on WiFi开关
 * @param layout 布局 (0=4应用, 1=2应用)
 * @param font_size 字体大小 (14/16/20/24)
 */
void sys_nvs_save_settings(int brightness, int volume, bool sound_on, int theme, 
                           bool wifi_on, int layout, int font_size);

/**
 * 从NVS加载设置
 * @param brightness 输出亮度
 * @param volume 输出音量
 * @param sound_on 输出声音开关
 * @param theme 输出主题
 * @param wifi_on 输出WiFi开关
 * @param layout 输出布局
 * @param font_size 输出字体大小
 * @return true成功加载，false使用默认值
 */
bool sys_nvs_load_settings(int *brightness, int *volume, bool *sound_on, int *theme,
                           bool *wifi_on, int *layout, int *font_size);

/**
 * 保存音频输出设备类型
 * @param audio_out 设备类型 (0=none, 1=buzzer, 2=i2s, 3=bt)
 */
void sys_nvs_save_audio_output(int audio_out);

/**
 * 加载音频输出设备类型
 * @return 设备类型，默认返回0（自动选择）
 */
int sys_nvs_load_audio_output(void);

/**
 * 保存音频自动选择模式
 * @param auto_mode true=自动, false=手动
 */
void sys_nvs_save_audio_auto(bool auto_mode);

/**
 * 加载音频自动选择模式
 * @return true=自动（默认），false=手动
 */
bool sys_nvs_load_audio_auto(void);

/**
 * 保存音量
 * @param volume 音量 (0-100)
 */
void sys_nvs_save_volume(int volume);

#endif /* SYS_NVS_H */