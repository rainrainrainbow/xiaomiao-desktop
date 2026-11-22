/**
 * @file drv_audio_i2s.h
 * @brief I2S DAC音频后端 - 通过I2S接口驱动外部DAC芯片
 *
 * 支持的DAC芯片（硬件无关，I2S标准信号）：
 * - NS4168 (数字功放，v71硬件)
 * - CS43131 (立创·hificat)
 * - ES7134LV / ES7148 / ES7243
 * - MAX98357 (I2S数字功放)
 * - PCM5102 / PCM5122
 * - UDA1334ATS
 *
 * 引脚连接（v71硬件，NS4168数字功放）：
 * - BCLK (位时钟) = GPIO25
 * - LRCLK (字选择) = GPIO32
 * - SDA (数据)  = GPIO33
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

/* ========== I2S引脚配置（v71硬件：NS4168数字功放） ========== */
#define I2S_BCK_PIN     GPIO_NUM_25     /* I2S位时钟 (Bit Clock) — NS4168 BCLK */
#define I2S_WS_PIN      GPIO_NUM_32     /* I2S字选择/帧同步 (Word Select / LRCLK) — NS4168 LRCLK */
#define I2S_DOUT_PIN    GPIO_NUM_33     /* I2S串行数据输出 (Data Out) — NS4168 SDA */

/* ========== I2S 写入诊断探针 ==========
 * 记录真正送入 i2s_channel_write() 的数据，用于区分：
 *   - 解码/展开正确但功放出噪声（硬件/格式问题）
 *   - 写入路径本身把数据写坏（软件问题）
 * 与解码器内部 probe 独立，本探针位于 mono->stereo 展开之后。 */
typedef struct {
    int      valid;        /* 是否采到过有效块 */
    int      count;        /* 有效样本数 */
    int16_t  samples[16];  /* 展开后送入 DMA 的前 16 个 int16 样本 */
    uint32_t writes;       /* 累计写入次数 */
    uint32_t last_len;     /* 最近一次写入字节数 */
    /* 数据供给及时性诊断（DMA underrun 假设的关键证据） */
    uint32_t max_gap_ms;   /* 两次 audio_output_write 之间的最大间隔(ms) */
    uint32_t slow_writes;  /* 间隔 > 25ms 的次数（超过 DMA 缓冲时长即会欠载） */
} i2s_write_probe_t;

void i2s_get_write_probe(i2s_write_probe_t *out);
void i2s_reset_write_probe(void);

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