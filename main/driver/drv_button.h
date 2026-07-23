/**
 * @file drv_button.h
 * @brief 按键驱动 - 6键手柄输入（带事件检测）
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
#define BUTTON_DEBOUNCE_MS   30

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
 * 按键任务（在独立任务中运行，检测按键事件）
 * @param pvParameters 未使用
 */
void drv_button_task(void *pvParameters);

/**
 * 获取按键事件（非阻塞）
 * @return 按下的按键索引（0-5），-1表示无事件
 * 
 * 注意：每个按键事件只返回一次，直到按键释放后再次按下
 */
int drv_button_get_event(void);

/**
 * 获取当前按下的按键（阻塞版本，用于调试）
 * @param timeout_ms 超时时间（毫秒）
 * @return 按下的按键索引，-1表示超时
 */
int drv_button_wait_press(uint32_t timeout_ms);

#endif /* DRV_BUTTON_H */