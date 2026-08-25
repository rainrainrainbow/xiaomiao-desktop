/**
 * @file drv_audio_i2s.c
 * @brief I2S DAC音频后端实现
 *
 * 使用ESP-IDF v5.3新I2S驱动（driver/i2s_std.h）驱动外部DAC芯片。
 * 支持标准Philips I2S格式，16/24/32位，单声道/立体声，8k~96kHz。
 *
 * 引脚（v71硬件）：BCK=GPIO25, LRCLK=GPIO32, DOUT=GPIO33
 * 适配：NS4168, CS43131, ES7134LV, MAX98357, PCM5102, UDA1334ATS
 */
#include "drv_audio_i2s.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "AUDIO_I2S";

/* ========== 内部状态 ========== */
static i2s_chan_handle_t s_i2s_tx_handle = NULL;
static bool s_i2s_initialized = false;
static bool s_i2s_opened = false;
static uint8_t s_i2s_volume = 50;
static uint32_t s_current_sample_rate = 44100;
static uint8_t s_current_bits = 16;
static uint8_t s_current_channels = 2;

/* ========== 音量处理 ========== */
static float volume_to_gain(uint8_t vol)
{
    if (vol == 0) return 0.0f;
    if (vol >= 100) return 1.0f;
    float normalized = (float)vol / 100.0f;
    return normalized * normalized; /* 平方映射，接近人耳感知 */
}

static void apply_volume_16bit(void *data, size_t len, float gain)
{
    int16_t *samples = (int16_t *)data;
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(samples[i] * gain);
    }
}

static void apply_volume_24bit(void *data, size_t len, float gain)
{
    uint8_t *bytes = (uint8_t *)data;
    size_t count = len / 3;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = (int8_t)(bytes[i * 3 + 2] & 0x80) ? 0xFF000000 : 0;
        sample |= bytes[i * 3] | (bytes[i * 3 + 1] << 8) | (bytes[i * 3 + 2] << 16);
        sample = (int32_t)(sample * gain);
        bytes[i * 3]     = sample & 0xFF;
        bytes[i * 3 + 1] = (sample >> 8) & 0xFF;
        bytes[i * 3 + 2] = (sample >> 16) & 0xFF;
    }
}

/* ========== 后端接口实现 ========== */

static esp_err_t i2s_backend_init(void)
{
    if (s_i2s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "I2S DAC backend ready (pins BCK=%d, WS=%d, DOUT=%d)",
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
    ESP_LOGI(TAG, "  I2S channel will be created on first open() call");

    s_i2s_initialized = true;
    return ESP_OK;
}

static void i2s_backend_deinit(void)
{
    if (!s_i2s_initialized) return;

    if (s_i2s_tx_handle) {
        if (s_i2s_opened) {
            i2s_channel_disable(s_i2s_tx_handle);
            s_i2s_opened = false;
        }
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
    }

    s_i2s_initialized = false;
    ESP_LOGI(TAG, "I2S backend deinitialized");
}

static esp_err_t i2s_backend_open(uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    if (!s_i2s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 如果已打开且参数相同，直接返回 */
    if (s_i2s_opened &&
        sample_rate == s_current_sample_rate &&
        bits == s_current_bits &&
        channels == s_current_channels) {
        return ESP_OK;
    }

    /* 如果已打开但参数不同，重建通道 */
    if (s_i2s_opened) {
        i2s_channel_disable(s_i2s_tx_handle);
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
        s_i2s_opened = false;
    }

    /* 参数验证与回退 */
    if (bits != 16 && bits != 24 && bits != 32) {
        ESP_LOGW(TAG, "Unsupported bits: %d, falling back to 16", bits);
        bits = 16;
    }
    if (channels != 1 && channels != 2) {
        ESP_LOGW(TAG, "Unsupported channels: %d, falling back to 2", channels);
        channels = 2;
    }
    if (sample_rate < 8000 || sample_rate > 96000) {
        ESP_LOGW(TAG, "Unsupported sample rate: %u Hz, falling back to 44100", (unsigned)sample_rate);
        sample_rate = 44100;
    }

    s_current_sample_rate = sample_rate;
    s_current_bits = bits;
    s_current_channels = channels;

    ESP_LOGI(TAG, "Opening I2S: %u Hz, %d bit, %d ch",
             (unsigned)sample_rate, bits, channels);

    /* ---- 创建I2S TX通道（ESP-IDF v5.3新API） ---- */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 256,
        .auto_clear = true,
    };

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- 配置I2S标准模式 ---- */
    i2s_data_bit_width_t bit_width;
    if (bits == 24) {
        bit_width = I2S_DATA_BIT_WIDTH_24BIT;
    } else if (bits == 32) {
        bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    } else {
        bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    }

    i2s_slot_mode_t slot_mode = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bit_width, slot_mode),
        .gpio_cfg = {
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,
            .invert_flags = {
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
        return ret;
    }

    /* ---- 启用通道 ---- */
    ret = i2s_channel_enable(s_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        i2s_del_channel(s_i2s_tx_handle);
        s_i2s_tx_handle = NULL;
        return ret;
    }

    s_i2s_opened = true;
    ESP_LOGI(TAG, "I2S opened successfully (DMA: %u desc x %u frames)",
             (unsigned)chan_cfg.dma_desc_num, (unsigned)chan_cfg.dma_frame_num);
    return ESP_OK;
}

static esp_err_t i2s_backend_write(const void *data, size_t len)
{
    if (!s_i2s_tx_handle || !s_i2s_opened) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float gain = volume_to_gain(s_i2s_volume);

    if (gain < 1.0f - 0.001f) {
        /* 需要应用音量调整，复制数据到临时缓冲区 */
        void *vol_buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!vol_buf) {
            ESP_LOGW(TAG, "Volume buffer alloc failed (%zu bytes), writing raw", len);
            size_t bytes_written = 0;
            return i2s_channel_write(s_i2s_tx_handle, data, len, &bytes_written, portMAX_DELAY);
        }

        memcpy(vol_buf, data, len);
        if (s_current_bits == 16) {
            apply_volume_16bit(vol_buf, len, gain);
        } else if (s_current_bits == 24) {
            apply_volume_24bit(vol_buf, len, gain);
        }
        /* 32位：直接乘法在i2s_channel_write中处理，暂不调整 */

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, vol_buf, len, &bytes_written, portMAX_DELAY);
        free(vol_buf);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write (vol) failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    /* 满音量，直接写入 */
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, data, len, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t i2s_backend_stop(void)
{
    if (!s_i2s_tx_handle || !s_i2s_opened) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 停止I2S：先disable再enable，清空DMA缓冲区 */
    esp_err_t ret = i2s_channel_disable(s_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable (after stop) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "I2S stopped and DMA flushed");
    return ESP_OK;
}

static void i2s_backend_flush(void)
{
    if (!s_i2s_tx_handle || !s_i2s_opened) return;

    /* 等待DMA缓冲区排空 */
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t i2s_backend_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_i2s_volume = vol;
    ESP_LOGD(TAG, "Volume set to %d (gain=%.3f)", vol, volume_to_gain(vol));
    return ESP_OK;
}

static uint8_t i2s_backend_get_volume(void)
{
    return s_i2s_volume;
}

static bool i2s_backend_is_available(void)
{
    /* I2S DAC始终可用（硬件直连，非热插拔） */
    return true;
}

/* ========== I2S后端实例 ========== */
const audio_backend_t s_i2s_backend = {
    .name = "I2S DAC",
    .type = AUDIO_OUT_I2S_DAC,
    .priority = 100,
    .init = i2s_backend_init,
    .deinit = i2s_backend_deinit,
    .open = i2s_backend_open,
    .write = i2s_backend_write,
    .stop = i2s_backend_stop,
    .flush = i2s_backend_flush,
    .set_volume = i2s_backend_set_volume,
    .get_volume = i2s_backend_get_volume,
    .is_available = i2s_backend_is_available,
};