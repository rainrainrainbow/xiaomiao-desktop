/**
 * @file drv_button.h
 * @brief 按键驱动 - 6键手柄输入
 */

#ifndef DRV_BUTTON_H
#define DRV_BUTTON_H

#include "driver/gpio.h"
#include <stdbool.h>

/* ========== 按键定义 ========== */
#define BTN_A          GPIO_NUM_34   /* ADC1_CH6 - SHARED with battery ADC! */
#define BTN_B          GPIO_NUM_12
#define BTN_UP         GPIO_NUM_2
#define BTN_DOWN       GPIO_NUM_13
#define BTN_LEFT       GPIO_NUM_27
#define BTN_RIGHT      GPIO_NUM_35

#define BUTTON_ACTIVE_LEVEL  0
#define BUTTON_DEBOUNCE_MS   25

/* ========== 按键索引 ========== */
typedef enum {
    BTN_IDX_UP = 0,
    BTN_IDX_DOWN,
    BTN_IDX_LEFT,
    BTN_IDX_RIGHT,
    BTN_IDX_A,
    BTN_IDX_B,
    BTN_IDX_MAX
} btn_idx_t;

/* ========== 按键驱动接口 ========== */

/**
 * 初始化按键驱动
 */
void drv_button_init(void);

/**
 * 扫描按键状态
 * @return 按下的按键索引，-1表示无按键
 */
int drv_button_scan(void);

/**
 * 获取按键GPIO电平
 * @param btn 按键索引
 * @return 电平状态
 */
bool drv_button_get_level(btn_idx_t btn);

#endif /* DRV_BUTTON_H */