/**
 * @file drv_mic_lmd2718.h
 * @brief LMD2718 数字麦克风驱动 - I2S 输入
 *
 * 硬件连接：
 * - DATA (SD) = GPIO21
 * - CLK  (SCK) = GPIO15
 *
 * LMD2718 是 PDM 输出数字麦克风，通过 I2S 接口接收 PDM 数据。
 * 使用 ESP32 I2S 控制器接收 PDM 信号，转换为 PCM 数据。
 */
#ifndef DRV_MIC_LMD2718_H
#define DRV_MIC_LMD2718_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 引脚定义 ========== */
#define MIC_DATA_PIN    GPIO_NUM_21     /* LMD2718 DATA (SD) */
#define MIC_CLK_PIN     GPIO_NUM_15     /* LMD2718 CLK (SCK) */

/* ========== 音频参数 ========== */
#define MIC_SAMPLE_RATE    16000       /* 采样率 16kHz */
#define MIC_BITS_PER_SAMPLE 16         /* 位深 16-bit */
#define MIC_CHANNELS        1          /* 单声道 */

/**
 * @brief 初始化 LMD2718 数字麦克风
 *
 * 配置 I2S 控制器为 PDM 接收模式，从 GPIO21/15 读取 PDM 数据。
 */
esp_err_t drv_mic_init(void);

/**
 * @brief 开始录音
 *
 * 启动 I2S 连续读取，数据通过回调函数返回。
 */
esp_err_t drv_mic_start(void);

/**
 * @brief 停止录音
 */
esp_err_t drv_mic_stop(void);

/**
 * @brief 读取一帧 PCM 数据（阻塞）
 * @param buffer 输出缓冲区
 * @param max_samples 最大样本数
 * @return 实际读取的样本数，0=无数据，<0=错误
 */
int drv_mic_read(int16_t *buffer, int max_samples);

/**
 * @brief 获取当前音量（RMS 电平，0-100）
 */
uint8_t drv_mic_get_level(void);

/**
 * @brief 麦克风是否已初始化
 */
bool drv_mic_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* DRV_MIC_LMD2718_H */