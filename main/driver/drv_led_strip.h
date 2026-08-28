/**
 * @file drv_led_strip.h
 * @brief NeoPixel RGB LED灯带驱动 - 与蜂鸣器复用GPIO14
 *
 * 硬件说明：
 * - 4颗 NeoPixel RGB（NEO_RGB 位序：R→G→B），连接在 GPIO14
 * - GPIO14 同时连接蜂鸣器（通过 LEDC PWM）
 * - 使用 RMT 驱动（时序与 WS2812B 兼容）
 * - 蜂鸣器使用时，LED 显示会短暂暂停
 *
 * 注意：
 * - NeoPixel RGB 和蜂鸣器共用 GPIO14，不能同时使用
 * - LED 驱动使用 RMT 发送时序信号
 * - 蜂鸣器使用 LEDC PWM 输出方波
 * - 使用时需互斥：LED 显示时停止蜂鸣器，反之亦然
 */
#ifndef DRV_LED_STRIP_H
#define DRV_LED_STRIP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LED数量 */
#define LED_STRIP_COUNT    3
/* 颜色结构体（NEO_RGB位序：R→G→B，NeoPixel RGB灯珠） */
typedef struct {
    uint8_t r;  /* 红色 */
    uint8_t g;  /* 绿色 */
    uint8_t b;  /* 蓝色 */
} led_rgb_t;

/**
 * @brief 初始化 NeoPixel RGB LED 灯带
 * 
 * 配置 RMT 控制器，设置 GPIO14 为输出。
 * 注意：会与蜂鸣器冲突，调用前需停止蜂鸣器。
 */
esp_err_t drv_led_strip_init(void);
/**
 * @brief 释放 NeoPixel RGB LED 灯带（停止 RMT 并释放 GPIO）
 *
 * 退出 LED 应用时调用，确保彻底熄灭并释放 GPIO14。
 */
esp_err_t drv_led_strip_deinit(void);

/**
 * @brief 设置单个 LED 颜色
 * @param index LED 索引（0-3）
 * @param color RGB 颜色值
 */
esp_err_t drv_led_strip_set_pixel(int index, led_rgb_t color);

/**
 * @brief 设置所有 LED 为同一颜色
 * @param color RGB 颜色值
 */
esp_err_t drv_led_strip_set_all(led_rgb_t color);

/**
 * @brief 刷新显示（将缓冲区数据发送到 LED 灯带）
 * 
 * 每次调用 set_pixel/set_all 后，需要调用 refresh 才能生效。
 */
esp_err_t drv_led_strip_refresh(void);

/**
 * @brief 关闭所有 LED
 */
esp_err_t drv_led_strip_clear(void);

/**
 * @brief 设置 LED 亮度（全局缩放）
 * @param brightness 亮度 0-255
 */
void drv_led_strip_set_brightness(uint8_t brightness);

/**
 * @brief 获取当前亮度
 */
uint8_t drv_led_strip_get_brightness(void);

/**
 * @brief 运行简单呼吸灯效果
 * @param color RGB 颜色
 * @param duration_ms 持续时间（ms），0=持续运行直到停止
 */
void drv_led_strip_breath(led_rgb_t color, uint32_t duration_ms);

/**
 * @brief 停止呼吸灯效果
 */
void drv_led_strip_breath_stop(void);

/**
 * @brief 检查 LED 是否正在使用（与蜂鸣器互斥）
 */
bool drv_led_strip_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_LED_STRIP_H */