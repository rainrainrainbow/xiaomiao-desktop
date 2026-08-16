/**
 * @file drv_buzzer.h
 * @brief 蜂鸣器驱动 - 通过LEDC PWM产生音调
 *
 * 使用 ESP32-S3 的 LEDC 定时器产生方波信号驱动有源/无源蜂鸣器。
 * 支持单音播放、音符频率映射（MIDI note number -> 频率）。
 */
#ifndef DRV_BUZZER_H
#define DRV_BUZZER_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/ledc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 蜂鸣器引脚定义 ========== */
#define PIN_BUZZER       GPIO_NUM_25   /* 蜂鸣器 PWM 引脚（ESP32-S3，未占用） */

#define BUZZER_TIMER     LEDC_TIMER_1  /* 使用独立定时器，避免与背光冲突 */
#define BUZZER_CHANNEL   LEDC_CHANNEL_1
#define BUZZER_DUTY_RES  LEDC_TIMER_10_BIT  /* 10-bit resolution (0-1023) */

/**
 * @brief 初始化蜂鸣器
 */
void drv_buzzer_init(void);

/**
 * @brief 播放指定频率的音调
 * @param freq_hz 频率（Hz），0 表示静音
 * @param duration_ms 持续时间（ms），0 表示持续播放直到停止
 */
void drv_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief 停止蜂鸣器
 */
void drv_buzzer_stop(void);

/**
 * @brief 将 MIDI 音符编号转换为频率
 * @param note MIDI 音符编号（0-127），如 60=C4, 69=A4(440Hz)
 * @return 频率（Hz）
 */
uint32_t drv_buzzer_note_to_freq(int note);

/**
 * @brief 播放一个 MIDI 音符
 * @param note MIDI 音符编号
 * @param duration_ms 持续时间（ms）
 */
void drv_buzzer_play_note(int note, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* DRV_BUZZER_H */