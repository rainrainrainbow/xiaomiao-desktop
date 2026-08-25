/**
 * @file drv_audio_decoder.c
 * @brief WAV音频解码器实现
 *
 * 解析RIFF WAVE文件格式，提取PCM音频数据。
 * 支持8位/16位PCM、单声道/立体声、8k~48kHz采样率。
 * 8位PCM自动转换为16位PCM输出，保持统一输出格式。
 *
 * RIFF WAVE文件结构：
 *   [RIFF HEADER] 12 bytes: "RIFF" + size + "WAVE"
 *   [fmt  chunk]  24+ bytes: "fmt " + size + format info
 *   [data chunk]  varies:    "data" + size + PCM samples
 *   [optional chunks] : "fact", "LIST", "cue ", etc.
 */
#include "drv_audio_decoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WAV_DEC";

/* ========== RIFF/WAVE 常量 ========== */
#define RIFF_ID       "RIFF"
#define WAVE_ID       "WAVE"
#define FMT_ID        "fmt "
#define DATA_ID       "data"

/* ========== 内部辅助函数 ========== */

/* 读取4字节小端uint32 */
static inline uint32_t read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* 读取2字节小端uint16 */
static inline uint16_t read_le16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/* 检查chunk ID是否匹配 */
static inline bool chunk_match(const uint8_t *buf, const char *id)
{
    return buf[0] == id[0] && buf[1] == id[1] &&
           buf[2] == id[2] && buf[3] == id[3];
}

/* ========== 公开API实现 ========== */

esp_err_t wav_decoder_init(wav_decoder_t *decoder)
{
    if (!decoder) return ESP_ERR_INVALID_ARG;

    memset(decoder, 0, sizeof(wav_decoder_t));
    decoder->state = WAV_DECODER_IDLE;
    decoder->file = NULL;

    ESP_LOGD(TAG, "Decoder initialized");
    return ESP_OK;
}

esp_err_t wav_decoder_open(wav_decoder_t *decoder, const char *filepath)
{
    if (!decoder || !filepath) return ESP_ERR_INVALID_ARG;
    if (decoder->state != WAV_DECODER_IDLE) {
        wav_decoder_close(decoder);
    }

    /* 打开文件 */
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    /* ---- 读取RIFF头 ---- */
    uint8_t header[12];
    if (fread(header, 1, 12, f) != 12) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        fclose(f);
        return ESP_FAIL;
    }

    /* 检查RIFF标志 */
    if (!chunk_match(header, RIFF_ID)) {
        ESP_LOGE(TAG, "Not a RIFF file: %c%c%c%c",
                 header[0], header[1], header[2], header[3]);
        fclose(f);
        return ESP_FAIL;
    }

    /* 检查WAVE标志 */
    if (!chunk_match(header + 8, WAVE_ID)) {
        ESP_LOGE(TAG, "Not a WAVE file (missing WAVE ID)");
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t riff_size = read_le32(header + 4);
    ESP_LOGD(TAG, "RIFF chunk size: %lu bytes", (unsigned long)riff_size);

    /* ---- 遍历chunks，查找fmt和data ---- */
    bool fmt_found = false;
    bool data_found = false;
    uint8_t chunk_hdr[8];

    while (1) {
        /* 读取chunk头（4字节ID + 4字节大小） */
        if (fread(chunk_hdr, 1, 8, f) != 8) {
            /* 文件末尾，正常结束 */
            break;
        }

        uint32_t chunk_size = read_le32(chunk_hdr + 4);

        if (chunk_match(chunk_hdr, FMT_ID)) {
            /* ---- 解析fmt chunk ---- */
            uint8_t fmt_buf[40];
            uint32_t fmt_read = (chunk_size > 40) ? 40 : chunk_size;
            if (fread(fmt_buf, 1, fmt_read, f) != fmt_read) {
                ESP_LOGE(TAG, "Failed to read fmt chunk");
                fclose(f);
                return ESP_FAIL;
            }

            decoder->format.format_tag      = read_le16(fmt_buf);
            decoder->format.channels        = read_le16(fmt_buf + 2);
            decoder->format.sample_rate     = read_le32(fmt_buf + 4);
            decoder->format.byte_rate       = read_le32(fmt_buf + 8);
            decoder->format.block_align     = read_le16(fmt_buf + 12);
            decoder->format.bits_per_sample = read_le16(fmt_buf + 14);

            /* 验证格式 */
            if (decoder->format.format_tag != 0x0001) {
                ESP_LOGE(TAG, "Unsupported format tag: 0x%04X (only PCM=0x0001 supported)",
                         decoder->format.format_tag);
                fclose(f);
                return ESP_FAIL;
            }

            if (decoder->format.channels != 1 && decoder->format.channels != 2) {
                ESP_LOGE(TAG, "Unsupported channels: %d (only mono/stereo)",
                         decoder->format.channels);
                fclose(f);
                return ESP_FAIL;
            }

            if (decoder->format.bits_per_sample != 8 &&
                decoder->format.bits_per_sample != 16) {
                ESP_LOGE(TAG, "Unsupported bits per sample: %d (only 8/16)",
                         decoder->format.bits_per_sample);
                fclose(f);
                return ESP_FAIL;
            }

            if (decoder->format.sample_rate < 8000 ||
                decoder->format.sample_rate > 48000) {
                ESP_LOGE(TAG, "Unsupported sample rate: %lu Hz (only 8k~48k)",
                         (unsigned long)decoder->format.sample_rate);
                fclose(f);
                return ESP_FAIL;
            }

            decoder->is_8bit = (decoder->format.bits_per_sample == 8);
            fmt_found = true;

            ESP_LOGI(TAG, "WAV format: %lu Hz, %d bit, %d ch, %lu bytes/sec",
                     (unsigned long)decoder->format.sample_rate,
                     decoder->format.bits_per_sample,
                     decoder->format.channels,
                     (unsigned long)decoder->format.byte_rate);

            /* 跳过剩余部分（如果chunk_size > 实际读取的） */
            if (chunk_size > fmt_read) {
                fseek(f, chunk_size - fmt_read, SEEK_CUR);
            }

        } else if (chunk_match(chunk_hdr, DATA_ID)) {
            /* ---- 解析data chunk ---- */
            decoder->format.data_size = chunk_size;
            decoder->format.data_offset = ftell(f);
            decoder->bytes_read = 0;

            /* 计算时长（毫秒） */
            if (decoder->format.byte_rate > 0) {
                decoder->format.duration_ms =
                    (uint32_t)((uint64_t)chunk_size * 1000 / decoder->format.byte_rate);
            } else {
                decoder->format.duration_ms = 0;
            }

            data_found = true;

            ESP_LOGI(TAG, "WAV data: %lu bytes, %lu ms, offset=%lu",
                     (unsigned long)chunk_size,
                     (unsigned long)decoder->format.duration_ms,
                     (unsigned long)decoder->format.data_offset);

            /* 找到data chunk，结束遍历 */
            break;

        } else {
            /* 跳过其他chunk */
            if (chunk_size > 0) {
                fseek(f, chunk_size, SEEK_CUR);
            }
            /* 如果chunk_size是奇数，RIFF要求padding字节 */
            if (chunk_size & 1) {
                fseek(f, 1, SEEK_CUR);
            }
        }
    }

    if (!fmt_found) {
        ESP_LOGE(TAG, "No fmt chunk found");
        fclose(f);
        return ESP_FAIL;
    }

    if (!data_found) {
        ESP_LOGE(TAG, "No data chunk found");
        fclose(f);
        return ESP_FAIL;
    }

    decoder->file = f;
    decoder->state = WAV_DECODER_READY;

    ESP_LOGI(TAG, "WAV file opened: %s (%lu ms, %lu Hz, %d bit, %d ch)",
             filepath,
             (unsigned long)decoder->format.duration_ms,
             (unsigned long)decoder->format.sample_rate,
             decoder->format.bits_per_sample,
             decoder->format.channels);

    return ESP_OK;
}

esp_err_t wav_decoder_read(wav_decoder_t *decoder, void *buffer,
                           size_t buffer_size, size_t *bytes_read)
{
    if (!decoder || !buffer || !bytes_read) return ESP_ERR_INVALID_ARG;

    *bytes_read = 0;

    if (decoder->state == WAV_DECODER_EOF) {
        return ESP_ERR_INVALID_STATE;
    }
    if (decoder->state != WAV_DECODER_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!decoder->file) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 计算剩余数据 */
    uint32_t remaining = decoder->format.data_size - decoder->bytes_read;
    if (remaining == 0) {
        decoder->state = WAV_DECODER_EOF;
        return ESP_ERR_INVALID_STATE;
    }

    if (decoder->is_8bit) {
        /* ---- 8位PCM：读取8位，转换为16位输出 ---- */
        /* 8位PCM是无符号偏移二进制（0-255，128=静音） */
        /* 16位PCM是有符号补码（-32768~32767） */
        /* 转换：s16 = (u8 - 128) << 8 */
        size_t max_samples = buffer_size / 2;  /* 输出为16位，每样本2字节 */
        if (max_samples > remaining) {
            max_samples = remaining;
        }

        uint8_t *u8_buf = (uint8_t *)heap_caps_malloc(max_samples, MALLOC_CAP_SPIRAM);
        if (!u8_buf) {
            ESP_LOGE(TAG, "Failed to allocate 8-bit buffer (%zu bytes)", max_samples);
            return ESP_ERR_NO_MEM;
        }

        size_t actual = fread(u8_buf, 1, max_samples, decoder->file);
        if (actual == 0) {
            free(u8_buf);
            decoder->state = WAV_DECODER_EOF;
            return ESP_ERR_INVALID_STATE;
        }

        int16_t *s16_buf = (int16_t *)buffer;
        for (size_t i = 0; i < actual; i++) {
            s16_buf[i] = (int16_t)(((int)u8_buf[i] - 128) << 8);
        }

        *bytes_read = actual * 2;
        decoder->bytes_read += actual;
        free(u8_buf);

    } else {
        /* ---- 16位PCM：直接读取 ---- */
        size_t to_read = buffer_size;
        if (to_read > remaining) {
            to_read = remaining;
        }
        /* 确保读取偶数个样本（16位对齐） */
        to_read &= ~1;

        if (to_read == 0) {
            decoder->state = WAV_DECODER_EOF;
            return ESP_ERR_INVALID_STATE;
        }

        size_t actual = fread(buffer, 1, to_read, decoder->file);
        if (actual == 0) {
            decoder->state = WAV_DECODER_EOF;
            return ESP_ERR_INVALID_STATE;
        }

        *bytes_read = actual;
        decoder->bytes_read += actual;
    }

    /* 检查是否已到末尾 */
    if (decoder->bytes_read >= decoder->format.data_size) {
        decoder->state = WAV_DECODER_EOF;
    }

    return ESP_OK;
}

esp_err_t wav_decoder_seek(wav_decoder_t *decoder, uint32_t position_ms)
{
    if (!decoder) return ESP_ERR_INVALID_ARG;
    if (decoder->state == WAV_DECODER_IDLE || !decoder->file) {
        return ESP_ERR_INVALID_STATE;
    }

    if (position_ms > decoder->format.duration_ms) {
        position_ms = decoder->format.duration_ms;
    }

    /* 计算目标字节偏移 */
    uint64_t byte_offset = (uint64_t)position_ms * decoder->format.byte_rate / 1000;
    /* 对齐到block_align */
    if (decoder->format.block_align > 0) {
        byte_offset = (byte_offset / decoder->format.block_align) * decoder->format.block_align;
    }
    if (byte_offset > decoder->format.data_size) {
        byte_offset = decoder->format.data_size;
    }

    /* 定位到data chunk中的偏移 */
    long pos = (long)(decoder->format.data_offset + byte_offset);
    if (fseek(decoder->file, pos, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Seek failed to position %lu", (unsigned long)pos);
        return ESP_FAIL;
    }

    decoder->bytes_read = (uint32_t)byte_offset;
    decoder->state = WAV_DECODER_READY;

    ESP_LOGD(TAG, "Seek to %lu ms (byte offset %lu/%lu)",
             (unsigned long)position_ms,
             (unsigned long)byte_offset,
             (unsigned long)decoder->format.data_size);

    return ESP_OK;
}

esp_err_t wav_decoder_close(wav_decoder_t *decoder)
{
    if (!decoder) return ESP_ERR_INVALID_ARG;

    if (decoder->file) {
        fclose(decoder->file);
        decoder->file = NULL;
    }

    decoder->state = WAV_DECODER_IDLE;
    memset(&decoder->format, 0, sizeof(wav_format_t));
    decoder->bytes_read = 0;

    ESP_LOGD(TAG, "Decoder closed");
    return ESP_OK;
}

const wav_format_t* wav_decoder_get_format(wav_decoder_t *decoder)
{
    if (!decoder) return NULL;
    if (decoder->state == WAV_DECODER_IDLE) return NULL;
    return &decoder->format;
}

wav_decoder_state_t wav_decoder_get_state(wav_decoder_t *decoder)
{
    if (!decoder) return WAV_DECODER_ERROR;
    return decoder->state;
}

uint32_t wav_decoder_get_position_ms(wav_decoder_t *decoder)
{
    if (!decoder || decoder->state == WAV_DECODER_IDLE) return 0;
    if (decoder->format.byte_rate == 0) return 0;
    return (uint32_t)((uint64_t)decoder->bytes_read * 1000 / decoder->format.byte_rate);
}

bool wav_decoder_is_wav_file(const char *filepath)
{
    if (!filepath) return false;

    /* 检查扩展名 */
    const char *ext = strrchr(filepath, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".wave") != 0) {
        return false;
    }

    /* 读取RIFF头验证 */
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    uint8_t header[12];
    if (fread(header, 1, 12, f) != 12) {
        fclose(f);
        return false;
    }
    fclose(f);

    return chunk_match(header, RIFF_ID) && chunk_match(header + 8, WAVE_ID);
}