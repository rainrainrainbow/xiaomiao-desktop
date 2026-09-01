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
#define NVS_KEY_FONT_SOURCE "font_src" // 字库来源 (0=FreeType, 1=内置)
#define NVS_KEY_LANGUAGE    "language"  // 语言 (0=中文, 1=English)
#define NVS_KEY_FONT_PATH   "font_path" // 字体文件路径编号或完整路径
#define NVS_KEY_MUSIC_EQ     "music_eq"   // 音乐频谱模式 (0=关, 1=开)
#define NVS_KEY_MUSIC_LRC    "music_lrc"  // 音乐歌词显示 (0=关, 1=开)
#define NVS_KEY_MUSIC_MODE   "music_mode" // 音乐循环模式 (0=单曲, 1=列表, 2=随机)
#define NVS_KEY_WIFI_SSID    "wifi_ssid"  // WiFi SSID
#define NVS_KEY_WIFI_PASS    "wifi_pass"  // WiFi 密码

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
 * 保存字库来源
 * @param font_source 字库来源 (0=FreeType/SD卡, 1=内置/英文)
 */
void sys_nvs_save_font_source(int font_source);

/**
 * 加载字库来源
 * @return 字库来源，默认返回0（FreeType）
 */
int sys_nvs_load_font_source(void);

/**
 * 保存语言
 * @param lang 语言 (0=中文, 1=English)
 */
void sys_nvs_save_language(int lang);

/**
 * 加载语言
 * @return 语言，默认返回0（中文）
 */
int sys_nvs_load_language(void);

/**
 * 保存当前字体文件路径索引
 * @param path_idx 字体索引 (0=自动/默认, 否则为扫描列表索引+1)
 */
void sys_nvs_save_font_path(int path_idx);

/**
 * 加载字体文件路径索引
 * @return 字体索引，默认返回0（自动）
 */
int sys_nvs_load_font_path(void);

/**
 * 保存音量
 * @param volume 音量 (0-100)
 */
void sys_nvs_save_volume(int volume);

/**
 * 保存音乐频谱模式
 * @param enabled 0=关闭, 1=开启
 */
void sys_nvs_save_music_eq(int enabled);

/**
 * 加载音乐频谱模式
 * @return 0=关闭（默认）, 1=开启
 */
int sys_nvs_load_music_eq(void);

/**
 * 保存音乐歌词显示
 * @param enabled 0=关闭, 1=开启
 */
void sys_nvs_save_music_lrc(int enabled);

/**
 * 加载音乐歌词显示
 * @return 0=关闭（默认）, 1=开启
 */
int sys_nvs_load_music_lrc(void);

/**
 * 保存音乐循环模式
 * @param mode 0=单曲循环, 1=列表循环, 2=随机播放
 */
void sys_nvs_save_music_mode(int mode);
/**
 * 加载音乐循环模式
 * @return 0=单曲（默认）, 1=列表, 2=随机
 */
int sys_nvs_load_music_mode(void);

/**
 * 保存 WiFi 凭据
 * @param ssid WiFi 名称（最大 32 字节）
 * @param password WiFi 密码（最大 64 字节）
 */
void sys_nvs_save_wifi_credentials(const char *ssid, const char *password);

/**
 * 加载 WiFi 凭据
 * @param ssid 输出缓冲区（至少 33 字节）
 * @param password 输出缓冲区（至少 65 字节）
 * @return true 成功加载，false 无保存的凭据
 */
bool sys_nvs_load_wifi_credentials(char *ssid, char *password);


#endif /* SYS_NVS_H */