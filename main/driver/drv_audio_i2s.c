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

/* ========== 写入诊断探针 ==========
 * 记录真正送入 i2s_channel_write() 的数据（mono->stereo 展开之后），
 * 独立于解码器内部 probe，用于区分"解码正确但写入/硬件破坏"与"解码源错"。 */
static i2s_write_probe_t s_write_probe;
static TickType_t s_last_write_tick = 0;
void i2s_get_write_probe(i2s_write_probe_t *out)
{
    if (out) *out = s_write_probe;
}
void i2s_reset_write_probe(void)
{
    memset(&s_write_probe, 0, sizeof(s_write_probe));
    s_last_write_tick = xTaskGetTickCount();
}
static void i2s_probe_capture(const void *data, size_t len)
{
    /* 数据供给间隔统计：若音乐任务/解码被 UI、FAT 或日志长期抢占，
     * 间隔超过 DMA 缓冲可覆盖的时长就会 underflow，表现为爆音/嘶声。 */
    TickType_t now = xTaskGetTickCount();
    if (s_last_write_tick != 0) {
        uint32_t gap_ms = (uint32_t)((now - s_last_write_tick) * portTICK_PERIOD_MS);
        if (gap_ms > s_write_probe.max_gap_ms) s_write_probe.max_gap_ms = gap_ms;
        if (gap_ms > 25) s_write_probe.slow_writes++;
    }
    s_last_write_tick = now;

    s_write_probe.writes++;
    s_write_probe.last_len = (uint32_t)len;
    if (s_write_probe.count > 0) return;  /* 只采首个非静音块 */
    const int16_t *s = (const int16_t *)data;
    size_t n = len / sizeof(int16_t);
    size_t start = 0;
    while (start < n && (s[start] == 0 || s[start] == -32768)) start++;  /* 跳过开头静音 */
    int pk = 0;
    for (size_t i = start; i < n; i++) {
        int a = s[i] < 0 ? -s[i] : s[i];
        if (a > pk) pk = a;
    }
    if (pk < 100) return;  /* 静音块不采 */
    size_t take = (n - start) < 16 ? (n - start) : 16;
    for (size_t i = 0; i < take; i++) s_write_probe.samples[i] = s[start + i];
    s_write_probe.count = (int)take;
    s_write_probe.valid = 1;
}

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

/* ========== I2S 数据写入说明（最终修复） ==========
 * 根因：经典 ESP32 I2S 在 16-bit MONO 模式下，硬件会把写入 buffer 中
 * 相邻的两个 int16 样本成对交换（ESP-IDF v5.3 官方 STD TX 文档明确记载）：
 *   buffer [s0,s1,s2,s3] → 线上 [s1,s0,s3,s2]。
 * 这解释了长期困扰的现象：低频正弦 tone 相邻样本高度相关，交换后听感
 * 几乎不变（"滴滴声正常"）；而 MP3/WAV 复杂波形相邻样本差异大，成对
 * 交换后波形被撕碎 → 满幅噪声（"电流声"）。解码数据本身始终正确。
 *
 * NS4168 数字功放实测要求标准 Philips I2S（bit_shift=true，数据相对 WS
 * 延迟 1 个 BCLK）。曾被误改为 MSB 对齐（bit_shift=false）导致采样位错位
 * 1 bit → MP3/WAV 退化为电流声，详见 最终交付报告.md。
 *
 * 最终方案（结合实机 I2S DMA 写入探针验证，两个条件必须同时满足）：
 *   1) I2S 通道始终以 STEREO 帧打开 → 彻底规避 16-bit MONO 样本对交换 quirk；
 *   2) 使用 Philips 宏 I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG → bit_shift=true，
 *      满足 NS4168 时序。
 * 逻辑单声道源在写入时展开为 L=R 双声道帧；立体声源交错数据直接写入。
 * tone 与音乐都走同一条无交换、时序正确的路径。 */

/* mono 源 → L=R 立体声帧展开（同时应用音量），规避 MONO 交换 quirk */
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

/* 持久复用的格式转换工作缓冲（PSRAM）：
 * 原先每次 write 都 malloc/free，小缓冲高频写入（512样本/次≈170次/秒）带来
 * 无谓的分配器抖动，是 TF 卡播放卡顿的帮凶之一。这里按需增长、跨调用复用，
 * 仅 deinit 释放；所有后端操作已由 s_i2s_lock 串行化，无并发冲突。 */
static void *s_work_buf = NULL;
static size_t s_work_buf_size = 0;

static void *i2s_work_buf_get(size_t need)
{
    if (s_work_buf && need <= s_work_buf_size) return s_work_buf;
    void *nb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nb) return NULL;   /* 旧缓冲保留，本调用失败但不破坏后续复用 */
    if (s_work_buf) heap_caps_free(s_work_buf);
    s_work_buf = nb;
    s_work_buf_size = need;
    return s_work_buf;
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

    if (s_work_buf) {
        heap_caps_free(s_work_buf);
        s_work_buf = NULL;
        s_work_buf_size = 0;
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

    /* ---- 创建I2S TX通道（DMA 描述符自适应降级） ----
     * i2s_alloc_dma_desc 需要在内部 RAM 的 DMA 池中循环分配
     * dma_desc_num 个 (lldesc_t + dma_frame_num*slot*bits/8) 块。
     * 系统长时间运行后内部 RAM 碎片化，大档位可能分配失败
     * (ESP_ERR_NO_MEM)。按 24→16→8→4 降级重试：
     * 24KB≈140ms、16KB≈93ms、8KB≈46ms@44.1k 立体声缓冲，
     * 配合写超时与 auto_clear 仍可稳定播放。缓冲越大，TF 卡
     * 补窗/解码抖动越不易造成欠载（卡顿/爆音）。 */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 24,
        .dma_frame_num = 256,
        .auto_clear = true,
    };

    static const uint32_t dma_desc_candidates[] = { 24, 16, 8, 4 };
    /* IDF v5.3 参数顺序为 (tx_handle, rx_handle)，不是 (rx, tx) */
    i2s_data_bit_width_t bit_width;
    if (bits == 24) {
        bit_width = I2S_DATA_BIT_WIDTH_24BIT;
    } else if (bits == 32) {
        bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    } else {
        bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    }

    /* 格式：标准 Philips I2S + 始终 STEREO 帧 + mono->L=R 展开。
     *
     * 依据（最终交付报告.md 记载的实机验证结论）：NS4168 数字功放要求
     * 标准 Philips I2S（数据相对 WS 延迟 1 个 BCLK，bit_shift=true）。
     * 曾有人以"参考设计"为由把 Philips 改回 MSB（commit 0d96d52），
     * 导致每个采样位错位 1 bit → 低频 tone 仍能听出音调、MP3/WAV 复杂
     * 波形退化为电流声。本轮 I2S DMA 写入探针已证明送入硬件的数据本身
     * 完全正确（tone 与音乐均为平滑 L=R 正弦），故问题只在时序格式。
     *
     * 恢复 Philips（bit_shift=true）+ STEREO 帧：
     *   - Philips：满足 NS4168 的 1-BCLK 延迟时序，消除位错位；
     *   - STEREO：规避经典 ESP32 16-bit MONO 样本对交换 quirk；
     *   - mono->L=R 展开：tone 与音乐共用同一条正确路径。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, I2S_SLOT_MODE_STEREO),
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

    esp_err_t ret = ESP_FAIL;
    for (size_t i = 0; i < sizeof(dma_desc_candidates) / sizeof(dma_desc_candidates[0]); i++) {
        chan_cfg.dma_desc_num = dma_desc_candidates[i];

        ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_handle, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_new_channel(%u desc) failed: %s",
                     (unsigned)chan_cfg.dma_desc_num, esp_err_to_name(ret));
            s_i2s_tx_handle = NULL;
            continue;
        }

        ret = i2s_channel_init_std_mode(s_i2s_tx_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_init_std_mode(%u desc) failed: %s",
                     (unsigned)chan_cfg.dma_desc_num, esp_err_to_name(ret));
            i2s_del_channel(s_i2s_tx_handle);
            s_i2s_tx_handle = NULL;
            continue;
        }

        ret = i2s_channel_enable(s_i2s_tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable(%u desc) failed: %s",
                     (unsigned)chan_cfg.dma_desc_num, esp_err_to_name(ret));
            i2s_del_channel(s_i2s_tx_handle);
            s_i2s_tx_handle = NULL;
            continue;
        }
        break;  /* 当前档位创建+初始化+启用全部成功 */
    }
    if (ret != ESP_OK || !s_i2s_tx_handle) {
        ESP_LOGE(TAG, "I2S channel create failed at all DMA sizes (24/16/8/4 desc)");
        return (ret == ESP_OK) ? ESP_FAIL : ret;
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

    /* 逻辑单声道 16-bit 源：展开为 L=R 立体声帧写入，规避 MONO 交换 quirk。
     * tone 与单声道 WAV/MP3 都走此分支；输出长度翻倍。 */
    if (s_current_channels == 1 && s_current_bits == 16) {
        size_t out_len = len * 2;
        void *work = i2s_work_buf_get(out_len);
        if (!work) {
            ESP_LOGW(TAG, "Stereo expand buffer alloc failed (%zu bytes)", out_len);
            return ESP_ERR_NO_MEM;
        }
        i2s_mono_to_stereo_16bit(data, work, len, gain, need_volume);
        i2s_probe_capture(work, out_len);  /* 诊断：采展开后真正写入 DMA 的数据 */
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, work, out_len,
                                          &bytes_written, pdMS_TO_TICKS(2000));
        if (ret != ESP_OK || bytes_written != out_len) {
            ESP_LOGE(TAG, "I2S write (mono->stereo) failed: %s written=%zu/%zu",
                     esp_err_to_name(ret), bytes_written, out_len);
        }
        return ret;
    }

    /* 立体声源（或其它位宽）：直接写入，按需应用音量 */
    if (need_volume) {
        void *work = i2s_work_buf_get(len);
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
        i2s_probe_capture(work, len);  /* 诊断 */
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(s_i2s_tx_handle, work, len, &bytes_written, pdMS_TO_TICKS(2000));
        return ret;
    }

    i2s_probe_capture(data, len);  /* 诊断 */
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