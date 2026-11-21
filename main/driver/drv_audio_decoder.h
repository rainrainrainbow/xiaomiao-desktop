/**
 * @file drv_audio_decoder.h
 * @brief 音频解码器 - 支持 WAV(PCM) 与 MP3，统一输出 16-bit PCM
 *
 * 支持的格式：
 * - WAV: PCM编码（WAVE_FORMAT_PCM = 0x0001），8/16位，单/立体声，8k~48kHz
 * - MP3: MPEG Layer I/II/III（minimp3 解码），通过文件魔数自动识别
 * - 所有格式统一输出 16-bit PCM，API 保持 wav_decoder_* 命名兼容既有调用
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

/* ========== 源类型 ========== */
typedef enum {
    AUDIO_SRC_UNKNOWN = 0,
    AUDIO_SRC_WAV,
    AUDIO_SRC_MP3
} audio_source_t;

/* ========== 解码器句柄 ========== */
typedef struct {
    wav_decoder_state_t state;  /* 解码器状态 */
    wav_format_t format;        /* 解码后PCM格式信息 */
    FILE *file;                 /* 文件句柄 */
    uint32_t bytes_read;        /* 已输出的PCM字节数 */
    bool is_8bit;               /* 是否8位PCM（需要转换为16位） */
    audio_source_t src;         /* 源文件类型（WAV/MP3） */

    /* ---- MP3 专用状态（WAV 时全部不占用） ---- */
    void *mp3_dec;              /* mp3dec_t*，从 PSRAM 分配 */
    void *mp3_file_buf;         /* 去 ID3 标签后的音频流起点（PSRAM，= raw+id3_skip） */
    void *mp3_raw_buf;          /* 原始分配基址（PSRAM，free 用；= raw） */
    size_t mp3_file_size;       /* 去标签后音频流大小 */
    size_t mp3_offset;          /* 当前读取位置（字节，指向 mp3_file_buf） */
    int16_t *mp3_pcm_buf;       /* 一帧解码 PCM 暂存（PSRAM） */
    size_t mp3_pcm_valid;       /* mp3_pcm_buf 中有效样本数 */
    size_t mp3_pcm_pos;         /* mp3_pcm_buf 已消费样本数 */
    bool mp3_byte_rate_est;     /* byte_rate 是否已估算 */

    /* ---- 只读诊断探针：记录最近一次 read 输出的首个非静音样本块 ---- */
    int16_t probe[16];
    size_t probe_count;
    int16_t probe_peak;     /* 首块内样本绝对值峰值，0=全静音 */
    size_t probe_offset;    /* 首个非静音样本在块内的下标 */
    int probe_first_peak;   /* open 首帧探测解码的 PCM 峰值（诊断解码器本身） */
    int first_samples[8];   /* open 首帧解码的前 8 个样本原值（诊断解码器输出） */
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

/**
 * @brief 检查文件是否为受支持的音频（WAV 或 MP3），按魔数识别
 * @param filepath 文件路径
 * @return true 是 WAV/MP3 且可解码
 */
bool audio_decoder_is_supported(const char *filepath);

/**
 * @brief 诊断 minimp3 常量表运行时读数（ESP32 PSRAM rodata 排查）
 * @param out 4 个整数：期望 [1000, -1, 999048, 785]
 */
void audio_decoder_get_mp3_table_probe(int out[4]);
void audio_decoder_get_mp3_deep_probe(int out[8]);

#ifdef __cplusplus
}
#endif

#endif /* DRV_AUDIO_DECODER_H */