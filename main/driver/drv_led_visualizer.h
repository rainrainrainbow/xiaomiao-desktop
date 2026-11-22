/**
 * @file drv_led_visualizer.h
 * @brief LED 音乐律动服务 — 随音乐 PCM 幅度驱动 NeoPixel 灯带
 *
 * 设计：
 * - 独立低优先级 FreeRTOS 任务，周期性读取 drv_audio_decoder 暴露的
 *   瞬时峰值 audio_decoder_get_live_peak()，经自适应增益 + 包络跟随后
 *   映射到灯带亮度/颜色，实现"随音乐律动"。
 * - 通过"占用标志"与 LED app 的手动效果互斥：LED app 有手动效果运行时，
 *   律动服务让位（不写灯），退出后自动恢复。
 * - 常驻持有一份 LED 驱动引用计数，避免 LED app 退出 deinit 释放 RMT。
 * - 模式与开关由设置页控制，经 NVS 持久化。
 */
#ifndef DRV_LED_VISUALIZER_H
#define DRV_LED_VISUALIZER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 律动模式 */
typedef enum {
    LED_VIS_MODE_OFF = 0,     /* 关闭：不律动 */
    LED_VIS_MODE_SINGLE = 1,  /* 单色律动：亮度随音量，颜色跟随设定色 */
    LED_VIS_MODE_RAINBOW = 2, /* 彩虹律动：色相随节拍推进，亮度随音量 */
    LED_VIS_MODE_PULSE = 3,   /* 节拍脉冲：底光常亮，节拍时整体泛白闪一下 */
    LED_VIS_MODE_MAX
} led_vis_mode_t;

/** 初始化并启动律动服务（幂等）。内部读取 NVS 中的模式/颜色配置。 */
esp_err_t led_visualizer_init(void);

/** 设置律动模式（0=关闭，1~3=各效果），立即生效并持久化到 NVS。 */
void led_visualizer_set_mode(int mode);

/** 获取当前律动模式（0..3）。 */
int  led_visualizer_get_mode(void);

/** 设置律动基色预设索引（对应 app_led 的 9 色调色板），立即生效并持久化。 */
void led_visualizer_set_color(int preset_index);

/** 获取当前律动基色预设索引。 */
int  led_visualizer_get_color(void);

/**
 * @brief LED app 手动效果占用/释放灯带。
 * 占用期间律动服务暂停写灯，避免与手动效果任务并发操作同一 RMT。
 */
void led_visualizer_set_busy(bool busy);

/** 律动服务是否处于激活状态（模式非关闭且未被 LED app 占用）。 */
bool led_visualizer_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_LED_VISUALIZER_H */
