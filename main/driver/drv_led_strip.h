/**
 * @file drv_led_strip.h
 * @brief NeoPixel RGB LED灯带驱动 - 官方 espressif/led_strip 组件（RMT）
 *
 * 硬件说明：
 * - 3颗 WS2812B NeoPixel RGB（GRB 位序：G→R→B），连接在 GPIO14
 * - 蜂鸣器已拆除，GPIO14 完全归 LED 使用（不再有 LEDC 抢占冲突）
 * - 使用官方 led_strip 组件驱动（内部封装 RMT 时序编码与复位信号）
 *
 * 注意：
 * - 官方组件 RMT 位序配置为 LED_STRIP_COLOR_COMPONENT_FMT_GRB（WS2812B 标准协议）
 * - RMT mem_block_symbols 须 ≥ 3*24+复位 ≈ 73（3颗灯），驱动内已配置 128
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
 * 使用官方 led_strip 组件配置 RMT，设置 GPIO14 为输出。
 * 蜂鸣器已拆除，无需互斥。
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