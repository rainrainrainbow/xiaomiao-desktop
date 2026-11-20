/**
 * @file wifi_debug.h
 * @brief WiFi 调试服务器 - 通过 TCP 端口输出 ESP_LOG
 */
#ifndef WIFI_DEBUG_H
#define WIFI_DEBUG_H
#include <stdbool.h>

/**
 * WiFi 自动连接
 * 从 NVS 加载凭据或使用默认凭据（tiger8/12345678）
 * 连接成功后启动调试服务器
 */
void wifi_auto_connect(void);

/**
 * 启动 WiFi 调试服务器
 * 监听端口 3333，将 ESP_LOG 输出转发到 TCP 客户端
 */
void wifi_debug_init(void);

/**
 * 停止 WiFi 调试服务器
 */
void wifi_debug_deinit(void);

/**
 * 检查是否有客户端连接
 * @return true 有客户端连接，false 无连接
 */
bool wifi_debug_is_connected(void);

/**
 * 暂停/恢复开机自动重连逻辑。
 * 当用户在 WiFi 设置页主动选择网络连接时，必须暂停自动重连，
 * 否则本模块的 STA_DISCONNECTED 处理器会用旧凭据不断 esp_wifi_connect()，
 * 覆盖用户正在建立的新连接，导致"连接失败/一直重试"。
 * @param pause true=暂停自动重连，false=恢复
 */
void wifi_debug_pause_autoreconnect(bool pause);

#endif /* WIFI_DEBUG_H */

