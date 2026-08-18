/**
 * @file drv_audio_bt_a2dp.c
 * @brief 蓝牙A2DP Sink后端实现
 *
 * 实现蓝牙A2DP Sink功能：
 * - 蓝牙控制器和Bluedroid协议栈初始化
 * - A2DP Sink接收手机音频
 * - AVRCP Controller播放控制
 * - PCM数据转发到I2S DAC播放
 *
 * 架构说明：
 * A2DP Sink是被动接收模式，不调用write()。
 * 接收的PCM数据通过回调直接转发到I2S DAC。
 */
#include "drv_audio_bt_a2dp.h"
#include "drv_audio_i2s.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_coexist.h"
#include <string.h>

static const char *TAG = "BT_A2DP";

/* ========== 状态变量 ========== */
static bool s_bt_initialized = false;
static bool s_a2dp_connected = false;
static bool s_a2dp_sink_started = false;
static char s_peer_name[32] = {0};

/* ========== 蓝牙GAP回调 ========== */
static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Authentication failed: %d", param->auth_cmpl.stat);
        }
        break;
    
    case ESP_BT_GAP_PIN_REQ_EVT:
        ESP_LOGI(TAG, "PIN request from %s", param->pin_req.bda);
        /* 使用默认PIN码 */
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 6, (uint8_t *)"123456");
        break;
    
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirmation request");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "Key notification: passkey=%lu", (unsigned long)param->key_notif.passkey);
        break;
    
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "Key request");
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 0);
        break;
    
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
        ESP_LOGI(TAG, "RSSI delta: %d", param->read_rssi_delta.rssi_delta);
        break;
    
    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

/* ========== A2DP Sink数据回调 ========== */
static void bt_a2dp_data_callback(const uint8_t *data, uint32_t len)
{
    if (!data || len == 0) {
        return;
    }
    
    /* 直接转发到I2S DAC播放 */
    /* 注意：这里假设I2S已初始化且配置正确 */
    /* 实际使用时需要确保I2S后端已打开 */
    if (s_i2s_backend.write) {
        s_i2s_backend.write(data, len);
    }
}

/* ========== A2DP Sink回调 ========== */
static void bt_a2dp_sink_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        bool connected = (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);
        s_a2dp_connected = connected;
        
        if (connected) {
            ESP_LOGI(TAG, "A2DP connected");
            /* 获取对端设备地址 */
            const uint8_t *bda = param->conn_stat.remote_bda;
            ESP_LOGI(TAG, "Remote device: %02x:%02x:%02x:%02x:%02x:%02x",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
            
            /* 通知音频输出系统 */
            audio_output_notify_device_change(AUDIO_OUT_BT_A2DP, true);
        } else {
            ESP_LOGI(TAG, "A2DP disconnected");
            audio_output_notify_device_change(AUDIO_OUT_BT_A2DP, false);
        }
        break;
    }
    
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "Audio state: %d", param->audio_stat.state);
        break;
    
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "Audio configuration received");
        break;
    
    default:
        ESP_LOGD(TAG, "A2DP event: %d", event);
        break;
    }
}

/* ========== AVRCP Controller回调 ========== */
static void bt_avrc_controller_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP connection state: %d", param->conn_stat.connected);
        break;
    
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGD(TAG, "AVRCP passthrough response received");
        break;
    
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(TAG, "AVRCP metadata response");
        /* 可以在这里处理歌曲信息 */
        break;
    
    default:
        ESP_LOGD(TAG, "AVRCP event: %d", event);
        break;
    }
}

/* ========== 后端接口实现 ========== */

static esp_err_t bt_a2dp_backend_init(void)
{
    if (s_bt_initialized) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing Bluetooth A2DP Sink...");
    
    /* 释放BLE内存（仅使用经典蓝牙） */
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE mem release failed: %s (may already be released)", esp_err_to_name(ret));
    }
    
    /* 初始化蓝牙控制器 */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* 启用蓝牙控制器 */
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        return ret;
    }
    
    /* 初始化Bluedroid协议栈 */
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }
    
    /* 注册GAP回调 */
    esp_bt_gap_register_callback(bt_gap_callback);
    
    /* 设置设备名称 */
    esp_bt_dev_set_device_name(BT_DEVICE_NAME);
    
    /* 设置可被发现模式 */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    
    /* 初始化A2DP Sink */
    ret = esp_a2d_register_callback(bt_a2dp_sink_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP register callback failed: %s", esp_err_to_name(ret));
    }
    
    ret = esp_a2d_sink_register_data_callback(bt_a2dp_data_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP register data callback failed: %s", esp_err_to_name(ret));
    }
    
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink init failed: %s", esp_err_to_name(ret));
    } else {
        s_a2dp_sink_started = true;
    }
    
    /* 初始化AVRCP Controller */
    ret = esp_avrc_ct_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP controller init failed: %s", esp_err_to_name(ret));
    }
    
    ret = esp_avrc_ct_register_callback(bt_avrc_controller_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP register callback failed: %s", esp_err_to_name(ret));
    }
    
    /* 启用蓝牙/WiFi共存 */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    
    s_bt_initialized = true;
    ESP_LOGI(TAG, "Bluetooth A2DP Sink initialized successfully");
    ESP_LOGI(TAG, "Device name: %s", BT_DEVICE_NAME);
    ESP_LOGI(TAG, "Device address: %s", esp_bt_dev_get_address());
    
    return ESP_OK;
}

static void bt_a2dp_backend_deinit(void)
{
    if (!s_bt_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing Bluetooth A2DP Sink...");
    
    /* 停止A2DP Sink */
    if (s_a2dp_sink_started) {
        esp_a2d_sink_deinit();
        s_a2dp_sink_started = false;
    }
    
    /* 停止AVRCP */
    esp_avrc_ct_deinit();
    
    /* 禁用Bluedroid */
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    
    /* 禁用蓝牙控制器 */
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    s_bt_initialized = false;
    s_a2dp_connected = false;
    ESP_LOGI(TAG, "Bluetooth A2DP Sink deinitialized");
}

static esp_err_t bt_a2dp_backend_open(uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    /* A2DP Sink是被动接收模式，open()不需要做任何事 */
    /* 音频参数由手机端决定，通过A2DP配置事件通知 */
    ESP_LOGD(TAG, "A2DP open: rate=%lu, bits=%d, ch=%d", 
             (unsigned long)sample_rate, bits, channels);
    return ESP_OK;
}

static esp_err_t bt_a2dp_backend_write(const void *data, size_t len)
{
    /* A2DP Sink不调用write()，数据通过回调接收 */
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bt_a2dp_backend_stop(void)
{
    /* A2DP Sink不需要主动停止 */
    return ESP_OK;
}

static void bt_a2dp_backend_flush(void)
{
    /* A2DP Sink不需要flush */
}

static esp_err_t bt_a2dp_backend_set_volume(uint8_t vol)
{
    /* A2DP Sink的音量控制需要发送AVRCP命令到手机端 */
    /* 这里简化处理，实际可以通过AVRCP Absolute Volume或Relative Volume命令 */
    ESP_LOGD(TAG, "A2DP set volume: %d", vol);
    
    /* TODO: 实现AVRCP音量控制命令 */
    /* esp_avrc_ct_send_set_absolute_volume_cmd(vol / 127); */
    
    return ESP_OK;
}

static uint8_t bt_a2dp_backend_get_volume(void)
{
    /* 返回当前音量（简化实现） */
    return 50;
}

static bool bt_a2dp_backend_is_available(void)
{
    /* 蓝牙已连接时可用 */
    return s_a2dp_connected;
}

/* ========== 后端实例 ========== */
const audio_backend_t s_bt_a2dp_backend = {
    .name = "Bluetooth A2DP",
    .type = AUDIO_OUT_BT_A2DP,
    .priority = 80,  /* 高于蜂鸣器(10)，低于I2S DAC(100) */
    .init = bt_a2dp_backend_init,
    .deinit = bt_a2dp_backend_deinit,
    .open = bt_a2dp_backend_open,
    .write = bt_a2dp_backend_write,
    .stop = bt_a2dp_backend_stop,
    .flush = bt_a2dp_backend_flush,
    .set_volume = bt_a2dp_backend_set_volume,
    .get_volume = bt_a2dp_backend_get_volume,
    .is_available = bt_a2dp_backend_is_available,
};

/* ========== 公开API ========== */

bool bt_a2dp_is_connected(void)
{
    return s_a2dp_connected;
}

esp_err_t bt_a2dp_get_peer_name(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!s_a2dp_connected) {
        strncpy(buf, "Not connected", len - 1);
        buf[len - 1] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(buf, s_peer_name, len - 1);
    buf[len - 1] = '\0';
    return ESP_OK;
}