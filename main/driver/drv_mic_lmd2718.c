/**
 * @file drv_mic_lmd2718.c
 * @brief LMD2718 数字麦克风驱动实现 - I2S PDM 输入
 *
 * LMD2718 是 PDM（脉冲密度调制）输出数字麦克风。
 * 使用 ESP32 I2S 控制器的 PDM 接收模式读取数据。
 *
 * 引脚：DATA=GPIO21, CLK=GPIO15
 *
 * 注意：
 * - ESP32 的 I2S 控制器支持 PDM 标准模式
 * - PDM 数据通过 I2S 接口接收，内部转换为 PCM
 * - 采样率 16kHz，16-bit，单声道
 */
#include "drv_mic_lmd2718.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "DRV_MIC";

/* ========== 内部状态 ========== */
static i2s_chan_handle_t s_mic_rx_handle = NULL;
static bool s_initialized = false;
static bool s_recording = false;
static uint8_t s_level = 0;  /* 当前音量电平 0-100 */

/* ========== 公共接口 ========== */

esp_err_t drv_mic_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing LMD2718 digital mic (DATA=GPIO%d, CLK=GPIO%d)",
             MIC_DATA_PIN, MIC_CLK_PIN);

    /* ---- 创建 I2S RX 通道 ---- */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,  /* 使用 I2S1 控制器（I2S0 用于音频输出） */
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 256,
        .auto_clear = true,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_mic_rx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- 配置 I2S 标准模式（PDM 接收） ---- */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = MIC_CLK_PIN,      /* CLK 作为位时钟 */
            .ws = I2S_GPIO_UNUSED,     /* 字选择未使用（PDM 模式） */
            .dout = I2S_GPIO_UNUSED,   /* 无输出 */
            .din = MIC_DATA_PIN,       /* DATA 作为数据输入 */
            .mclk = I2S_GPIO_UNUSED,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_mic_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_mic_rx_handle);
        s_mic_rx_handle = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LMD2718 digital mic initialized (16kHz, 16-bit, mono)");
    return ESP_OK;
}

esp_err_t drv_mic_start(void)
{
    if (!s_initialized || !s_mic_rx_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recording) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_enable(s_mic_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_recording = true;
    ESP_LOGI(TAG, "Mic recording started");
    return ESP_OK;
}

esp_err_t drv_mic_stop(void)
{
    if (!s_initialized || !s_mic_rx_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_recording) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_mic_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_recording = false;
    ESP_LOGI(TAG, "Mic recording stopped");
    return ESP_OK;
}

int drv_mic_read(int16_t *buffer, int max_samples)
{
    if (!s_initialized || !s_recording || !buffer || max_samples <= 0) {
        return -1;
    }

    size_t bytes_read = 0;
    size_t bytes_to_read = max_samples * sizeof(int16_t);

    esp_err_t ret = i2s_channel_read(s_mic_rx_handle, buffer, bytes_to_read,
                                      &bytes_read, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(ret));
        return -1;
    }

    int samples = bytes_read / sizeof(int16_t);

    /* 计算 RMS 电平 */
    if (samples > 0) {
        double sum_sq = 0;
        for (int i = 0; i < samples; i++) {
            sum_sq += (double)buffer[i] * buffer[i];
        }
        double rms = sqrt(sum_sq / samples);
        /* 映射到 0-100 */
        s_level = (uint8_t)(rms / 327.68);  /* 32768/100 */
        if (s_level > 100) s_level = 100;
    }

    return samples;
}

uint8_t drv_mic_get_level(void)
{
    return s_level;
}

bool drv_mic_is_initialized(void)
{
    return s_initialized;
}