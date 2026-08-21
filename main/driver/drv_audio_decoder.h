/**
 * @file drv_audio_decoder.h
 * @brief WAV音频解码器 - 解析RIFF WAVE文件并提取PCM数据
 *
 * 支持的格式：
 * - PCM编码（WAVE_FORMAT_PCM = 0x0001）
 * - 8位/16位采样
 * - 单声道/立体声
 * - 采样率：8kHz ~ 48kHz
 *
 * 使用方式：
 * @code
 *   wav_decoder_t wav;
 *   if (wav_decoder_init(&wav) == ESP_OK &&
 *       wav_decoder_open(&wav, "/sdcard/music/test.wav") == ESP_OK) {
 *       int16_t buf[1024];
 *       size_t read = 0;
 *       while (wav_decoder_read(&wav, buf, sizeof(buf), &read) == ESP_OK && read > 0) {
 *           audio_output_write(buf, read);
 *       }
 *       wav_decoder_close(&wav);
 *   }
 * @endcode
 */
#ifndef DRV_AUDIO_DECODER_H
#define DRV_AUDIO_DECODER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== WAV格式信息 ========== */
typedef struct {
    uint16_t format_tag;        /* 编码格式（1=PCM） */
    uint16_t channels;          /* 声道数（1=单声道, 2=立体声） */
    uint32_t sample_rate;       /* 采样率（Hz） */
    uint32_t byte_rate;         /* 数据率（字节/秒） */
    uint16_t block_align;       /* 数据块对齐（字节） */
    uint16_t bits_per_sample;   /* 位深（8/16） */
    uint32_t data_size;         /* PCM数据总长度（字节） */
    uint32_t data_offset;       /* PCM数据起始偏移（文件头） */
    uint32_t duration_ms;       /* 音频时长（毫秒） */
} wav_format_t;

/* ========== 解码器状态 ========== */
typedef enum {
    WAV_DECODER_IDLE = 0,       /* 空闲/未初始化 */
    WAV_DECODER_READY,          /* 已打开文件，可读取 */
    WAV_DECODER_EOF,            /* 已读取到文件末尾 */
    WAV_DECODER_ERROR           /* 错误状态 */
} wav_decoder_state_t;

/* ========== 解码器句柄 ========== */
typedef struct {
    wav_decoder_state_t state;  /* 解码器状态 */
    wav_format_t format;        /* WAV格式信息 */
    FILE *file;                 /* 文件句柄 */
    uint32_t bytes_read;        /* 已读取的PCM字节数 */
    bool is_8bit;               /* 是否8位PCM（需要转换为16位） */
} wav_decoder_t;

/* ========== API函数 ========== */

/**
 * @brief 初始化WAV解码器
 * @param decoder 解码器句柄指针
 * @return ESP_OK 成功
 */
esp_err_t wav_decoder_init(wav_decoder_t *decoder);

/**
 * @brief 打开WAV文件并解析头部
 * @param decoder 解码器句柄指针
 * @param filepath WAV文件路径
 * @return ESP_OK 成功
 *         ESP_FAIL 文件格式错误或不支持
 *         ESP_ERR_NOT_FOUND 文件不存在
 */
esp_err_t wav_decoder_open(wav_decoder_t *decoder, const char *filepath);

/**
 * @brief 读取PCM数据块
 * @param decoder 解码器句柄指针
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小（字节）
 * @param bytes_read 实际读取的字节数（输出）
 * @return ESP_OK 正常读取
 *         ESP_ERR_INVALID_STATE 解码器未打开或已到末尾
 */
esp_err_t wav_decoder_read(wav_decoder_t *decoder, void *buffer, size_t buffer_size, size_t *bytes_read);

/**
 * @brief 跳转到指定位置（毫秒）
 * @param decoder 解码器句柄指针
 * @param position_ms 目标位置（毫秒）
 * @return ESP_OK 成功
 */
esp_err_t wav_decoder_seek(wav_decoder_t *decoder, uint32_t position_ms);

/**
 * @brief 关闭WAV解码器
 * @param decoder 解码器句柄指针
 * @return ESP_OK 成功
 */
esp_err_t wav_decoder_close(wav_decoder_t *decoder);

/**
 * @brief 获取WAV格式信息
 * @param decoder 解码器句柄指针
 * @return wav_format_t 格式信息指针，NULL如果未打开
 */
const wav_format_t* wav_decoder_get_format(wav_decoder_t *decoder);

/**
 * @brief 获取解码器状态
 * @param decoder 解码器句柄指针
 * @return 当前状态
 */
wav_decoder_state_t wav_decoder_get_state(wav_decoder_t *decoder);

/**
 * @brief 获取当前播放进度（毫秒）
 * @param decoder 解码器句柄指针
 * @return 已播放的毫秒数
 */
uint32_t wav_decoder_get_position_ms(wav_decoder_t *decoder);

/**
 * @brief 检查文件是否为WAV格式
 * @param filepath 文件路径
 * @return true 是WAV格式
 */
bool wav_decoder_is_wav_file(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_DECODER_H */