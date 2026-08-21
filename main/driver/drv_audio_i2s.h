/**
 * @file drv_audio_i2s.h
 * @brief I2S DAC音频后端 - 通过I2S接口驱动外部DAC芯片
 *
 * 支持的DAC芯片（硬件无关，I2S标准信号）：
 * - CS43131 (立创·hificat)
 * - ES7134LV / ES7148 / ES7243
 * - MAX98357 (I2S数字功放)
 * - PCM5102 / PCM5122
 * - UDA1334ATS
 *
 * 引脚连接（默认，可在配置中修改）：
 * - BCK (位时钟) = GPIO26
 * - WS  (字选择) = GPIO25  
 * - DOUT (数据)  = GPIO33
 * - MCLK (主时钟) = 未使用（DAC内部PLL或外部晶振）
 *
 * ESP32 I2S0 控制器：
 * - 支持标准Philips I2S格式
 * - 16/24/32位数据宽度
 * - 单声道/立体声
 * - 采样率 8kHz ~ 96kHz
 */
#ifndef DRV_AUDIO_I2S_H
#define DRV_AUDIO_I2S_H

#include "driver/drv_audio_output.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== I2S引脚配置 ========== */
#define I2S_BCK_PIN     GPIO_NUM_26     /* I2S位时钟 (Bit Clock) */
#define I2S_WS_PIN      GPIO_NUM_25     /* I2S字选择/帧同步 (Word Select / LRCLK) */
#define I2S_DOUT_PIN    GPIO_NUM_33     /* I2S串行数据输出 (Data Out) */

/* ========== I2S后端实例 ========== */

/**
 * @brief I2S DAC后端实例
 * 
 * 在 drv_audio_output.c 的 audio_output_init() 中注册：
 * @code
 *   audio_output_register_backend(&s_i2s_backend);
 * @endcode
 * 
 * 优先级：100（高于蜂鸣器10和蓝牙A2DP 80）
 */
extern const audio_backend_t s_i2s_backend;

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_I2S_H */