/**
 * @file drv_backlight.h
 * @brief 背光驱动 - PWM控制LCD背光
 */

#ifndef DRV_BACKLIGHT_H
#define DRV_BACKLIGHT_H

#include "driver/gpio.h"

/* ========== 背光引脚定义 ========== */
/* 
 * 注意：GPIO_NUM_0 (GPIO0) 是 ESP32 的启动模式选择引脚。
 * 用作背光 PWM 输出时，需确保背光电路在启动时不影响 GPIO0 电平，
 * 否则可能导致 ESP32 进入下载模式而非正常启动。
 * 如果硬件允许，建议迁移到非启动关键引脚（如 GPIO15）。
 */
#define PIN_LCD_BL     GPIO_NUM_0      /* Backlight PWM（⚠ GPIO0 是启动引脚） */

/* ========== 蜂鸣器引脚定义（LEDC PWM） ========== */
#define PIN_BUZZER     GPIO_NUM_14     /* 蜂鸣器 PWM 引脚 */

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_13_BIT  /* 13-bit resolution (0-8191) */
#define LEDC_FREQ_HZ        5000               /* 5kHz PWM frequency */

/* ========== 背光驱动接口 ========== */

/**
 * 初始化背光PWM
 */
void drv_backlight_init(void);

/**
 * 设置背光亮度
 * @param percent 亮度百分比 (0-100)
 */
void drv_backlight_set_brightness(int percent);

/**
 * 获取当前亮度
 * @return 亮度百分比
 */
int drv_backlight_get_brightness(void);

#endif /* DRV_BACKLIGHT_H */