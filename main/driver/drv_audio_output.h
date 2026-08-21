/**
 * @file drv_audio_output.h
 * @brief 音频输出抽象层 - 统一音频输出API
 *
 * 支持三种输出后端：
 * - BUZZER:  LEDC PWM 蜂鸣器（始终可用，保底设备）
 * - I2S_DAC: I2S 外部DAC芯片（CS43131/ES7134LV/MAX98357等）
 * - BT_A2DP: 蓝牙A2DP Sink（需蓝牙设备连接）
 *
 * 自动搜索策略：
 * - 启动时自动检测所有可用后端
 * - 按优先级自动选择：I2S(100) > BT(80) > Buzzer(10)
 * - 用户可在设置中手动选择，NVS持久化
 * - BT热插拔时自动切换（自动模式下）
 */
#ifndef DRV_AUDIO_OUTPUT_H
#define DRV_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 设备类型 ========== */
typedef enum {
    AUDIO_OUT_NONE     = 0,
    AUDIO_OUT_BUZZER   = 1,   /* LEDC PWM 蜂鸣器 */
    AUDIO_OUT_I2S_DAC  = 2,   /* I2S 外部DAC */
    AUDIO_OUT_BT_A2DP  = 3,   /* 蓝牙A2DP Sink */
    AUDIO_OUT_MAX
} audio_out_type_t;

/* ========== 设备信息 ========== */
typedef struct {
    audio_out_type_t type;
    char name[32];             /* 显示名称 */
    bool available;            /* 当前是否可用 */
    bool is_default;           /* 是否为当前输出 */
    uint32_t sample_rate;      /* 支持的采样率（蜂鸣器=0） */
    uint8_t channels;          /* 声道数（蜂鸣器=1） */
    uint8_t bits_per_sample;   /* 位深（蜂鸣器=0） */
    int priority;              /* 自动选择优先级（越高越优先） */
} audio_device_info_t;

/* ========== 后端驱动接口（vtable） ========== */
typedef struct audio_backend {
    const char *name;
    audio_out_type_t type;
    int priority;

    /* 生命周期 */
    esp_err_t (*init)(void);
    void (*deinit)(void);

    /* PCM播放 */
    esp_err_t (*open)(uint32_t sample_rate, uint8_t bits, uint8_t channels);
    esp_err_t (*write)(const void *data, size_t len);
    esp_err_t (*stop)(void);
    void (*flush)(void);

    /* 音量 */
    esp_err_t (*set_volume)(uint8_t vol);
    uint8_t (*get_volume)(void);

    /* 状态 */
    bool (*is_available)(void);
} audio_backend_t;

/* ========== 初始化/反初始化 ========== */

/**
 * @brief 初始化音频输出系统
 * 
 * 扫描并注册所有可用后端，按优先级自动选择默认设备。
 * 应在 drv_buzzer_init() 之后调用。
 */
esp_err_t audio_output_init(void);

/**
 * @brief 反初始化音频输出系统
 */
void audio_output_deinit(void);

/* ========== 设备管理 ========== */

/**
 * @brief 获取所有已发现设备
 * @param devs 输出缓冲区
 * @param max_count 最大设备数
 * @return 实际设备数
 */
int audio_output_get_devices(audio_device_info_t *devs, int max_count);

/**
 * @brief 获取当前活跃设备类型
 */
audio_out_type_t audio_output_get_active(void);

/**
 * @brief 切换到指定设备（用户手动选择）
 * @param type 目标设备类型
 * @return ESP_OK 成功, ESP_ERR_NOT_FOUND 设备不可用
 */
esp_err_t audio_output_set_active(audio_out_type_t type);

/**
 * @brief 自动选择最佳可用设备（按优先级）
 */
esp_err_t audio_output_auto_select(void);

/**
 * @brief 是否为自动选择模式
 */
bool audio_output_is_auto_mode(void);

/**
 * @brief 设置自动/手动模式
 * @param auto_mode true=自动, false=手动
 */
void audio_output_set_auto_mode(bool auto_mode);

/* ========== PCM播放控制 ========== */

/**
 * @brief 打开音频流
 * @param sample_rate 采样率（如 44100, 48000, 16000）
 * @param bits 位深（16, 24, 32）
 * @param channels 声道数（1=单声道, 2=立体声）
 */
esp_err_t audio_output_open(uint32_t sample_rate, uint8_t bits, uint8_t channels);

/**
 * @brief 写入PCM数据
 * @param data PCM数据缓冲区
 * @param len 数据长度（字节）
 */
esp_err_t audio_output_write(const void *data, size_t len);

/**
 * @brief 停止播放
 */
esp_err_t audio_output_stop(void);

/**
 * @brief 清空缓冲区
 */
void audio_output_flush(void);

/* ========== 音量控制 ========== */

/**
 * @brief 设置音量
 * @param vol 音量 0-100
 */
esp_err_t audio_output_set_volume(uint8_t vol);

/**
 * @brief 获取当前音量
 * @return 音量 0-100
 */
uint8_t audio_output_get_volume(void);

/* ========== 蜂鸣器兼容API ========== */

/**
 * @brief 播放单音（兼容蜂鸣器模式）
 * 
 * 蜂鸣器后端：直接调用 drv_buzzer_tone()
 * DAC/蓝牙后端：合成对应频率的正弦波PCM
 * 
 * @param freq_hz 频率（Hz），0=静音
 * @param duration_ms 持续时间（ms），0=持续到停止
 */
void audio_output_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief 播放MIDI音符（兼容蜂鸣器模式）
 * @param note MIDI音符编号（0-127）
 * @param duration_ms 持续时间（ms）
 */
void audio_output_play_note(int note, uint32_t duration_ms);

/* ========== 事件通知（后端调用） ========== */

/**
 * @brief 通知设备状态变化（后端驱动调用）
 * @param type 设备类型
 * @param available 是否可用
 */
void audio_output_notify_device_change(audio_out_type_t type, bool available);

/**
 * @brief 注册后端驱动
 * @param backend 后端驱动指针（静态分配，不可释放）
 */
esp_err_t audio_output_register_backend(const audio_backend_t *backend);

/**
 * @brief 定时轮询（主循环调用，检测热插拔）
 */
void audio_output_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_OUTPUT_H */
