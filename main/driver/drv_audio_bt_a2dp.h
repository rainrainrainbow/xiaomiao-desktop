/**
 * @file drv_audio_bt_a2dp.h
 * @brief 蓝牙A2DP Sink后端 - 接收手机音频并播放
 *
 * 功能：
 * - 蓝牙经典模式控制器初始化
 * - A2DP Sink 角色（接收音频流）
 * - AVRCP 控制器（播放/暂停/音量控制）
 * - PCM数据接收后转发到I2S DAC播放
 *
 * 连接流程：
 * 1. 设备上电后蓝牙可被发现（设备名：Xiaomiao Desktop）
 * 2. 手机蓝牙搜索并连接
 * 3. 手机播放音乐，音频通过A2DP传输到ESP32
 * 4. ESP32接收PCM数据，通过I2S DAC播放
 *
 * 注意：
 * - 蓝牙和WiFi共享射频，启用共存（coexistence）
 * - A2DP需要较大内存（~50KB），使用PSRAM分配
 * - 仅支持ESP32（非S2/S3/C3，无经典蓝牙）
 */
#ifndef DRV_AUDIO_BT_A2DP_H
#define DRV_AUDIO_BT_A2DP_H

#include "drv_audio_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 蓝牙设备配置 ========== */
#define BT_DEVICE_NAME          "Xiaomiao Desktop"  /* 蓝牙显示名称 */
#define BT_DISCOVERABLE         true                 /* 启动后可被发现 */

/* ========== A2DP后端实例 ========== */

/**
 * @brief 蓝牙A2DP Sink后端实例
 * 
 * 在 drv_audio_output.c 的 audio_output_init() 中注册：
 * @code
 *   audio_output_register_backend(&s_bt_a2dp_backend);
 * @endcode
 * 
 * 优先级：80（高于蜂鸣器10，低于I2S DAC 100）
 * 
 * 注意：A2DP Sink是被动接收模式，write()不会被调用。
 * 接收的PCM数据直接转发到I2S DAC播放。
 */
extern const audio_backend_t s_bt_a2dp_backend;

/* ========== 状态查询 ========== */

/**
 * @brief 查询蓝牙A2DP是否已连接
 * @return true=已连接, false=未连接
 */
bool bt_a2dp_is_connected(void);

/**
 * @brief 获取对端蓝牙设备名称
 * @param buf 输出缓冲区
 * @param len 缓冲区长度
 * @return ESP_OK 成功
 */
esp_err_t bt_a2dp_get_peer_name(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_BT_A2DP_H */
