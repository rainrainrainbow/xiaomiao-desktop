/**
 * @file ion/button.h
 * @brief Ion - Hardware Abstraction Layer: Button Interface
 * 
 * 参考 NumWorks Epsilon 的 Ion 层设计，提供统一的按键接口。
 * 支持 6 键手柄：UP, DOWN, LEFT, RIGHT, A, B
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>

/* 按键枚举 */
typedef enum {
    ION_BUTTON_UP = 0,
    ION_BUTTON_DOWN,
    ION_BUTTON_LEFT,
    ION_BUTTON_RIGHT,
    ION_BUTTON_A,      /* ADC 读取（GPIO34） */
    ION_BUTTON_B,      /* GPIO12 */
    ION_BUTTON_COUNT
} ion_button_t;

/* 按键状态 */
typedef enum {
    ION_BUTTON_STATE_RELEASED = 0,
    ION_BUTTON_STATE_PRESSED,
    ION_BUTTON_STATE_LONG_PRESS  /* 长按（>1秒） */
} ion_button_state_t;

/**
 * @brief 初始化按键驱动
 * @return true 成功，false 失败
 */
bool ion_button_init(void);

/**
 * @brief 扫描所有按键状态
 * @note 应在主循环中定期调用（如每 10ms）
 */
void ion_button_scan(void);

/**
 * @brief 获取指定按键的当前状态
 * @param button 按键枚举
 * @return 按键状态
 */
ion_button_state_t ion_button_get_state(ion_button_t button);

/**
 * @brief 检查是否有按键被按下
 * @return true 有按键按下，false 无按键
 */
bool ion_button_any_pressed(void);

/**
 * @brief 获取最后按下的按键
 * @return 按键枚举，ION_BUTTON_COUNT 表示无按键
 */
ion_button_t ion_button_last_pressed(void);

/**
 * @brief 等待按键事件（阻塞）
 * @param timeout_ms 超时时间（毫秒），0 表示无限等待
 * @return 按下的按键枚举，ION_BUTTON_COUNT 表示超时
 */
ion_button_t ion_button_wait(uint32_t timeout_ms);

#endif /* BUTTON_H */