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
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

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

/* ========== MONO 源 → STEREO I2S 帧展开 ==========
 * NS4168 是标准 Philips I2S 器件，其 CTRL 引脚选择左/右声道。
 * 经典 ESP32 的 I2S STD TX 在 8/16-bit MONO 模式有芯片级 quirk：
 *   "for 8-bit and 16-bit mono modes, the real data on the line is swapped.
 *    To get the correct data sequence, the writing buffer needs to swap the data every two bytes."
 * 且 MONO 模式只向单个 slot 写数据，另一个 slot 恒为 0 —— 若 NS4168 的
 * CTRL 恰好选中空 slot，或相邻样本被交换，复杂波形（MP3）就会退化为噪声，
 * 而低频正弦（tone）因相邻样本接近仍能听出音调。
 *
 * 彻底规避方案：I2S 通道始终用 STEREO 帧打开，把每个 mono 样本同时写入
 * 左右两个 slot（L=R=sample）。这样线上顺序与 buffer 一致（无交换 quirk），
 * 两个 slot 都有有效数据（与 CTRL 选左/选右无关），NS4168 参考设计亦如此。 */
static void i2s_mono_to_stereo_16bit(const void *src, void *dst, size_t len,
                                    float gain, bool apply_gain)
{
    const int16_t *s = (const int16_t *)src;
    int16_t *d = (int16_t *)dst;
    size_t n = len / sizeof(int16_t);
    for (size_t i = 0; i < n; i++) {
        int32_t v = s[i];
        if (apply_gain) v = (int32_t)(v * gain);
        int16_t vv = (int16_t)v;
        d[i * 2]     = vv;   /* left  */
        d[i * 2 + 1] = vv;   /* right */
    }
}

/* ========== 后端接口实现 ========== */

static esp_err_t i2s_backend_init_locked(void)
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

static void i2s_backend_deinit_locked(void)
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

static esp_err_t i2s_backend_open_locked(uint32_t sample_rate, uint8_t bits, uint8_t channels)
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

    /* IDF v5.3 参数顺序为 (tx_handle, rx_handle)，不是 (rx, tx) */
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, NULL);
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

    /* 始终以 STEREO 帧打开 I2S。逻辑单声道源在写入时展开为 L=R 双声道，
     * 规避经典 ESP32 MONO 模式的样本对交换 quirk 与空 slot 问题（见上方说明）。 */
    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;

    /* NS4168 数字功放要求标准 Philips I2S：数据在 LRCLK 变化后延迟 1 个 BCLK
     * 才输出 MSB（datasheet: "支持 I2S 格式(BCLK 延迟一个时钟)"）。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode),
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

static esp_err_t i2s_backend_write_locked(const void *data, size_t len)
{
    if (!s_i2s_tx_handle || !s_i2s_opened) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float gain = volume_to_gain(s_i2s_volume);
    bool need_volume = (gain < 1.0f - 0.001f);
    /* 逻辑单声道 16-bit 源需要展开为 L=R 双声道 STEREO 帧 */
    bool mono_expand = (s_current_channels == 1 && s_current_bits == 16);

    if (mono_expand) {
        /* 输出长度翻倍：每个 int16 样本 → 左右两个 int16 */
        size_t out_len = len * 2;
        void *work = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!work) {
            ESP_LOGW(TAG, "Stereo work buffer alloc failed (%zu bytes)", out_len);
            return ESP_ERR_NO_MEM;
        }
        i2s_mono_to_stereo_16bit(data, work, len, gain, need_volume);

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, work, out_len,
                                          &bytes_written, pdMS_TO_TICKS(2000));
        heap_caps_free(work);
        if (ret != ESP_OK || bytes_written != out_len) {
            ESP_LOGE(TAG, "I2S write (stereo) failed: %s written=%zu/%zu",
                     esp_err_to_name(ret), bytes_written, out_len);
        }
        return ret;
    }

    /* 立体声源：直接写，按需应用音量 */
    if (need_volume) {
        void *work = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!work) {
            ESP_LOGW(TAG, "Volume buffer alloc failed (%zu bytes), writing raw", len);
            size_t bytes_written = 0;
            return i2s_channel_write(s_i2s_tx_handle, data, len, &bytes_written, pdMS_TO_TICKS(2000));
        }
        memcpy(work, data, len);
        if (s_current_bits == 16) {
            apply_volume_16bit(work, len, gain);
        } else if (s_current_bits == 24) {
            apply_volume_24bit(work, len, gain);
        }
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, work, len, &bytes_written, pdMS_TO_TICKS(2000));
        heap_caps_free(work);
        return ret;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, data, len, &bytes_written, pdMS_TO_TICKS(2000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t i2s_backend_stop_locked(void)
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

static void i2s_backend_flush_locked(void)
{
    if (!s_i2s_tx_handle || !s_i2s_opened) return;

    /* 等待DMA缓冲区排空 */
    vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t i2s_backend_set_volume_locked(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_i2s_volume = vol;
    ESP_LOGD(TAG, "Volume set to %d (gain=%.3f)", vol, volume_to_gain(vol));
    return ESP_OK;
}

static uint8_t i2s_backend_get_volume_locked(void)
{
    return s_i2s_volume;
}

static bool i2s_backend_is_available_locked(void)
{
    /* I2S DAC始终可用（硬件直连，非热插拔） */
    return true;
}

/* ========== I2S 后端操作串行化 ==========
 * 快速连续播放 tone 时，open/write/stop/deinit 可能被不同任务并发调用。
 * 若 write 期间通道被删除并重建，旧句柄可能指向 RX 通道，
 * 触发 i2s_channel_write(): "this channel is not tx channel" 刷屏并淹没 UART 响应。
 * 所有通道操作统一持锁，写超时改为 2s，避免长期占锁。 */
static SemaphoreHandle_t s_i2s_lock = NULL;

static esp_err_t i2s_backend_init(void)
{
    if (!s_i2s_lock) {
        s_i2s_lock = xSemaphoreCreateMutex();
        if (!s_i2s_lock) return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = i2s_backend_init_locked();
    xSemaphoreGive(s_i2s_lock);
    return ret;
}

static void i2s_backend_deinit(void)
{
    if (!s_i2s_lock) return;
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return;
    i2s_backend_deinit_locked();
    xSemaphoreGive(s_i2s_lock);
}

static esp_err_t i2s_backend_open(uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    if (!s_i2s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = i2s_backend_open_locked(sample_rate, bits, channels);
    xSemaphoreGive(s_i2s_lock);
    return ret;
}

static esp_err_t i2s_backend_write(const void *data, size_t len)
{
    if (!s_i2s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = i2s_backend_write_locked(data, len);
    xSemaphoreGive(s_i2s_lock);
    return ret;
}

static esp_err_t i2s_backend_stop(void)
{
    if (!s_i2s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = i2s_backend_stop_locked();
    xSemaphoreGive(s_i2s_lock);
    return ret;
}

static void i2s_backend_flush(void)
{
    if (!s_i2s_lock) return;
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return;
    i2s_backend_flush_locked();
    xSemaphoreGive(s_i2s_lock);
}

static esp_err_t i2s_backend_set_volume(uint8_t vol)
{
    if (!s_i2s_lock) {
        s_i2s_volume = vol > 100 ? 100 : vol;
        return ESP_OK;
    }
    if (xSemaphoreTake(s_i2s_lock, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t ret = i2s_backend_set_volume_locked(vol);
    xSemaphoreGive(s_i2s_lock);
    return ret;
}

static uint8_t i2s_backend_get_volume(void)
{
    return s_i2s_volume;
}

static bool i2s_backend_is_available(void)
{
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