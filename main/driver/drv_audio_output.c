/**
 * @file drv_audio_output.c
 * @brief 音频输出抽象层 - 路由层实现
 *
 * 管理多个音频后端，提供统一的播放API。
 * 支持自动搜索设备、用户手动选择、热插拔检测。
 */
#include "drv_audio_output.h"
#include "drv_buzzer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

static const char *TAG = "AUDIO_OUT";

/* ========== 后端注册表 ========== */
#define MAX_BACKENDS 4
static const audio_backend_t *s_backends[MAX_BACKENDS] = {0};
static int s_backend_count = 0;

/* ========== 当前状态 ========== */
static const audio_backend_t *s_active_backend = NULL;
static audio_out_type_t s_active_type = AUDIO_OUT_NONE;
static bool s_auto_mode = true;  // 默认自动模式
static uint8_t s_volume = 50;    // 全局音量 0-100
static bool s_initialized = false;

/* ========== 热插拔检测 ========== */
static bool s_bt_was_connected = false;  /* 用于Phase 3蓝牙热插拔检测 */
static int64_t s_last_poll_time = 0;
#define POLL_INTERVAL_MS 5000  // 5秒轮询一次

/* ========== 正弦波合成（用于DAC模式下的tone/note） ========== */
static uint32_t s_sine_phase = 0;
static uint32_t s_sine_freq = 0;
static uint32_t s_sine_sample_rate = 44100;

/* 合成单帧正弦波PCM（16bit mono） */
static int16_t synth_sine_sample(void)
{
    if (s_sine_freq == 0) return 0;
    
    /* 相位累加器 */
    uint32_t phase_inc = (uint32_t)((uint64_t)s_sine_freq * 0xFFFFFFFF / s_sine_sample_rate);
    s_sine_phase += phase_inc;
    
    /* 相位转角度（0~2π） */
    float angle = (float)(s_sine_phase >> 16) / 65536.0f * 2.0f * M_PI;
    
    /* 生成样本（音量衰减） */
    float sample = sinf(angle) * 0.3f * (s_volume / 100.0f);
    return (int16_t)(sample * 32767);
}

/* ========== 蜂鸣器后端（内置，始终可用） ========== */
static esp_err_t buzzer_backend_init(void)
{
    /* 蜂鸣器已在 drv_buzzer_init() 中初始化 */
    return ESP_OK;
}

static void buzzer_backend_deinit(void)
{
    drv_buzzer_stop();
}

static esp_err_t buzzer_backend_open(uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    /* 蜂鸣器不支持PCM流，但记录采样率用于tone合成 */
    s_sine_sample_rate = sample_rate > 0 ? sample_rate : 44100;
    return ESP_OK;
}

static esp_err_t buzzer_backend_write(const void *data, size_t len)
{
    /* 蜂鸣器不支持PCM写入，忽略 */
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t buzzer_backend_stop(void)
{
    drv_buzzer_stop();
    s_sine_freq = 0;
    return ESP_OK;
}

static void buzzer_backend_flush(void)
{
    /* 蜂鸣器无缓冲 */
}

static esp_err_t buzzer_backend_set_volume(uint8_t vol)
{
    drv_buzzer_set_volume(vol);
    return ESP_OK;
}

static uint8_t buzzer_backend_get_volume(void)
{
    return drv_buzzer_get_volume();
}

static bool buzzer_backend_is_available(void)
{
    return true;  /* 蜂鸣器始终可用 */
}

static const audio_backend_t s_buzzer_backend = {
    .name = "Buzzer",
    .type = AUDIO_OUT_BUZZER,
    .priority = 10,
    .init = buzzer_backend_init,
    .deinit = buzzer_backend_deinit,
    .open = buzzer_backend_open,
    .write = buzzer_backend_write,
    .stop = buzzer_backend_stop,
    .flush = buzzer_backend_flush,
    .set_volume = buzzer_backend_set_volume,
    .get_volume = buzzer_backend_get_volume,
    .is_available = buzzer_backend_is_available,
};

/* ========== 内部函数 ========== */

/* 查找指定类型的后端 */
static const audio_backend_t *find_backend(audio_out_type_t type)
{
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i] && s_backends[i]->type == type) {
            return s_backends[i];
        }
    }
    return NULL;
}

/* 获取最高优先级的可用后端 */
static const audio_backend_t *find_best_available(void)
{
    const audio_backend_t *best = NULL;
    int best_priority = -1;
    
    for (int i = 0; i < s_backend_count; i++) {
        const audio_backend_t *b = s_backends[i];
        if (b && b->is_available && b->is_available()) {
            if (b->priority > best_priority) {
                best = b;
                best_priority = b->priority;
            }
        }
    }
    return best;
}

/* 切换到指定后端（内部） */
static esp_err_t switch_to_backend(const audio_backend_t *backend)
{
    if (!backend) {
        return ESP_ERR_NOT_FOUND;
    }
    
    /* 停止当前后端 */
    if (s_active_backend && s_active_backend->stop) {
        s_active_backend->stop();
    }
    
    /* 切换 */
    s_active_backend = backend;
    s_active_type = backend->type;
    
    /* 同步音量 */
    if (backend->set_volume) {
        backend->set_volume(s_volume);
    }
    
    ESP_LOGI(TAG, "Switched to: %s (priority=%d)", backend->name, backend->priority);
    return ESP_OK;
}

/* ========== 公开API实现 ========== */

esp_err_t audio_output_register_backend(const audio_backend_t *backend)
{
    if (!backend || s_backend_count >= MAX_BACKENDS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* 检查是否已注册 */
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i] == backend || s_backends[i]->type == backend->type) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    
    s_backends[s_backend_count++] = backend;
    ESP_LOGI(TAG, "Registered backend: %s (type=%d, priority=%d)", 
             backend->name, backend->type, backend->priority);
    return ESP_OK;
}

esp_err_t audio_output_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing audio output system...");
    
    /* 注册蜂鸣器后端（始终可用） */
    audio_output_register_backend(&s_buzzer_backend);
    
    /* TODO: Phase 2 - 探测I2S DAC */
    /* if (CONFIG_AUDIO_I2S_ENABLED) {
     *     audio_output_register_backend(&s_i2s_backend);
     * }
     */
    
    /* TODO: Phase 3 - 初始化蓝牙A2DP */
    /* if (CONFIG_AUDIO_BT_A2DP_ENABLED) {
     *     audio_output_register_backend(&s_bt_a2dp_backend);
     * }
     */
    
    /* 初始化所有后端 */
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->init) {
            esp_err_t ret = s_backends[i]->init();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Backend %s init failed: %s", 
                         s_backends[i]->name, esp_err_to_name(ret));
            }
        }
    }
    
    /* 自动选择最佳设备 */
    audio_output_auto_select();
    
    s_initialized = true;
    s_last_poll_time = esp_timer_get_time() / 1000;  // 转为毫秒
    
    ESP_LOGI(TAG, "Audio output initialized, %d backends, active=%s",
             s_backend_count, s_active_backend ? s_active_backend->name : "none");
    return ESP_OK;
}

void audio_output_deinit(void)
{
    if (!s_initialized) return;
    
    /* 停止当前后端 */
    if (s_active_backend && s_active_backend->stop) {
        s_active_backend->stop();
    }
    
    /* 反初始化所有后端 */
    for (int i = 0; i < s_backend_count; i++) {
        if (s_backends[i]->deinit) {
            s_backends[i]->deinit();
        }
    }
    
    s_backend_count = 0;
    s_active_backend = NULL;
    s_active_type = AUDIO_OUT_NONE;
    s_initialized = false;
}

int audio_output_get_devices(audio_device_info_t *devs, int max_count)
{
    if (!devs || max_count <= 0) return 0;
    
    int count = 0;
    for (int i = 0; i < s_backend_count && count < max_count; i++) {
        const audio_backend_t *b = s_backends[i];
        if (!b) continue;
        
        audio_device_info_t *info = &devs[count++];
        info->type = b->type;
        strncpy(info->name, b->name, sizeof(info->name) - 1);
        info->name[sizeof(info->name) - 1] = '\0';
        info->available = b->is_available ? b->is_available() : false;
        info->is_default = (b->type == s_active_type);
        info->priority = b->priority;
        
        /* 根据类型填充能力信息 */
        switch (b->type) {
            case AUDIO_OUT_BUZZER:
                info->sample_rate = 0;
                info->channels = 1;
                info->bits_per_sample = 0;
                break;
            case AUDIO_OUT_I2S_DAC:
                info->sample_rate = 44100;  // 默认
                info->channels = 2;
                info->bits_per_sample = 16;
                break;
            case AUDIO_OUT_BT_A2DP:
                info->sample_rate = 44100;
                info->channels = 2;
                info->bits_per_sample = 16;
                break;
            default:
                break;
        }
    }
    return count;
}

audio_out_type_t audio_output_get_active(void)
{
    return s_active_type;
}

esp_err_t audio_output_set_active(audio_out_type_t type)
{
    const audio_backend_t *backend = find_backend(type);
    if (!backend) {
        ESP_LOGW(TAG, "Backend type %d not found", type);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (backend->is_available && !backend->is_available()) {
        ESP_LOGW(TAG, "Backend %s not available", backend->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    /* 切换到手动模式 */
    s_auto_mode = false;
    
    return switch_to_backend(backend);
}

esp_err_t audio_output_auto_select(void)
{
    const audio_backend_t *best = find_best_available();
    if (!best) {
        ESP_LOGE(TAG, "No available backend found!");
        return ESP_ERR_NOT_FOUND;
    }
    
    s_auto_mode = true;
    return switch_to_backend(best);
}

bool audio_output_is_auto_mode(void)
{
    return s_auto_mode;
}

void audio_output_set_auto_mode(bool auto_mode)
{
    s_auto_mode = auto_mode;
    if (auto_mode) {
        audio_output_auto_select();
    }
}

/* ========== PCM播放控制 ========== */

esp_err_t audio_output_open(uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    if (!s_active_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_active_backend->open) {
        return s_active_backend->open(sample_rate, bits, channels);
    }
    return ESP_OK;
}

esp_err_t audio_output_write(const void *data, size_t len)
{
    if (!s_active_backend || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_active_backend->write) {
        return s_active_backend->write(data, len);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_output_stop(void)
{
    if (!s_active_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    
    s_sine_freq = 0;
    
    if (s_active_backend->stop) {
        return s_active_backend->stop();
    }
    return ESP_OK;
}

void audio_output_flush(void)
{
    if (s_active_backend && s_active_backend->flush) {
        s_active_backend->flush();
    }
}

/* ========== 音量控制 ========== */

esp_err_t audio_output_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;
    
    if (s_active_backend && s_active_backend->set_volume) {
        return s_active_backend->set_volume(vol);
    }
    return ESP_OK;
}

uint8_t audio_output_get_volume(void)
{
    return s_volume;
}

/* ========== 蜂鸣器兼容API ========== */

void audio_output_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_active_backend) return;
    
    if (s_active_type == AUDIO_OUT_BUZZER) {
        /* 蜂鸣器模式：直接调用 */
        drv_buzzer_tone(freq_hz, duration_ms);
    } else {
        /* DAC模式：合成正弦波 */
        s_sine_freq = freq_hz;
        s_sine_phase = 0;
        
        if (freq_hz == 0) {
            /* 静音 */
            return;
        }
        
        /* 打开音频流（如果未打开） */
        if (s_active_backend->open) {
            s_active_backend->open(s_sine_sample_rate, 16, 1);
        }
        
        if (duration_ms == 0) {
            /* 持续播放，由stop()停止 */
            return;
        }
        
        /* 计算需要生成的样本数 */
        uint32_t samples = (uint32_t)((uint64_t)s_sine_sample_rate * duration_ms / 1000);
        size_t buf_size = 256;  // 每次写入256个样本
        int16_t *buf = heap_caps_malloc(buf_size * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "Failed to allocate sine buffer");
            return;
        }
        
        uint32_t remaining = samples;
        while (remaining > 0) {
            size_t chunk = remaining > buf_size ? buf_size : remaining;
            for (size_t i = 0; i < chunk; i++) {
                buf[i] = synth_sine_sample();
            }
            if (s_active_backend->write) {
                s_active_backend->write(buf, chunk * sizeof(int16_t));
            }
            remaining -= chunk;
        }
        
        free(buf);
        s_sine_freq = 0;
    }
}

void audio_output_play_note(int note, uint32_t duration_ms)
{
    uint32_t freq = drv_buzzer_note_to_freq(note);
    audio_output_tone(freq, duration_ms);
}

/* ========== 事件通知 ========== */

void audio_output_notify_device_change(audio_out_type_t type, bool available)
{
    ESP_LOGI(TAG, "Device change: type=%d, available=%d", type, available);
    
    if (!s_initialized) return;
    
    if (s_auto_mode) {
        /* 自动模式下，重新选择最佳设备 */
        const audio_backend_t *best = find_best_available();
        if (best && best->type != s_active_type) {
            switch_to_backend(best);
        }
    }
}

/* ========== 热插拔轮询 ========== */

void audio_output_poll(void)
{
    if (!s_initialized) return;
    
    int64_t now = esp_timer_get_time() / 1000;  // 毫秒
    if (now - s_last_poll_time < POLL_INTERVAL_MS) {
        return;
    }
    s_last_poll_time = now;
    
    /* 检查蓝牙连接状态（Phase 3实现） */
    /* TODO:
     * bool bt_connected = bt_a2dp_is_connected();
     * if (bt_connected != s_bt_was_connected) {
     *     audio_output_notify_device_change(AUDIO_OUT_BT_A2DP, bt_connected);
     *     s_bt_was_connected = bt_connected;
     * }
     */
}