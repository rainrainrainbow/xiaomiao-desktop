/**
 * @file drv_backlight.h
 * @brief 背光驱动 - PWM控制LCD背光
 */

#ifndef DRV_BACKLIGHT_H
#define DRV_BACKLIGHT_H

#include "driver/gpio.h"

/* ========== 背光引脚定义 ========== */
#define PIN_LCD_BL     GPIO_NUM_0      /* Backlight PWM */

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