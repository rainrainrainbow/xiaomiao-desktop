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
#define NVS_KEY_SOUND       "sound"
#define NVS_KEY_THEME       "theme"
#define NVS_KEY_WIFI        "wifi"
#define NVS_KEY_LAYOUT      "layout"

/* ========== NVS存储接口 ========== */

/**
 * 初始化NVS存储
 * @return 0成功，其他失败
 */
int sys_nvs_init(void);

/**
 * 保存设置到NVS
 * @param brightness 亮度 (10-100)
 * @param sound_on 声音开关
 * @param theme 主题 (0=Dark, 1=Light)
 * @param wifi_on WiFi开关
 * @param layout 布局 (0=4应用, 1=2应用)
 */
void sys_nvs_save_settings(int brightness, bool sound_on, int theme, 
                           bool wifi_on, int layout);

/**
 * 从NVS加载设置
 * @param brightness 输出亮度
 * @param sound_on 输出声音开关
 * @param theme 输出主题
 * @param wifi_on 输出WiFi开关
 * @param layout 输出布局
 * @return true成功加载，false使用默认值
 */
bool sys_nvs_load_settings(int *brightness, bool *sound_on, int *theme,
                           bool *wifi_on, int *layout);

#endif /* SYS_NVS_H */