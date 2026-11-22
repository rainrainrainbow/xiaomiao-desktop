/**
 * @file drv_audio_decoder.c
 * @brief 音频解码器实现：WAV(PCM) + MP3，统一输出 16-bit PCM
 *
 * WAV: 解析 RIFF/WAVE，提取 PCM。支持 8/16 位、单/立体声、8k~48kHz。
 * MP3: minimp3 (CC0) 解码 MPEG Layer I/II/III，按文件魔数自动识别。
 *      原始 mp3 数据一次性载入 PSRAM，逐帧解码，避免 flash 反复读取与cache抖动。
 *
 * 全部公开 API 保持 wav_decoder_* 命名，内部按 decoder->src 分派，
 * 使调用方（app_music.c）无需感知格式差异。
 */
#include "drv_audio_decoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

/* minimp3 公共领域单头文件解码器；仅本翻译单元实例化实现 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
/* 【关键】ESP32 Xtensa 无 SSE/NEON。不定义 NO_SIMD 时 minimp3 平台检测
 * 可能误选不适用的 SIMD/浮点路径，导致解码输出几乎全 0（播放电流声/静音）。
 * 强制纯标量 C 路径（Meta/Arduino-ESP32 的 minimp3 移植都这么做）。 */
#define MINIMP3_NO_SIMD
/* ESP32: 把 ~16KB 解码 scratch 从任务栈移到 PSRAM，避免栈溢出静默踩坏 PCM。
 * scratch 在 mp3dec_decode_frame 内懒分配一次复用。 */
#define MINIMP3_PSRAM_SCRATCH
#include "minimp3.h"

static const char *TAG = "AUDIO_DEC";

/* ========== RIFF/WAVE 常量 ========== */
#define RIFF_ID       "RIFF"
#define WAVE_ID       "WAVE"
#define FMT_ID        "fmt "
#define DATA_ID       "data"

/* ========== 内部辅助函数 ========== */

static inline uint32_t read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline uint16_t read_le16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline bool chunk_match(const uint8_t *buf, const char *id)
{
    return buf[0] == id[0] && buf[1] == id[1] &&
           buf[2] == id[2] && buf[3] == id[3];
}

/* 探测文件类型：读取头部魔数 */
static audio_source_t detect_source(FILE *f)
{
    uint8_t h[12];
    size_t n = fread(h, 1, sizeof(h), f);
    rewind(f);
    if (n >= 12 && chunk_match(h, RIFF_ID) && chunk_match(h + 8, WAVE_ID))
        return AUDIO_SRC_WAV;
    if (n >= 3) {
        /* ID3v2 容器 */
        if (h[0] == 'I' && h[1] == 'D' && h[2] == '3')
            return AUDIO_SRC_MP3;
        /* MPEG 帧同步：0xFF + (111xxxxx) */
        if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)
            return AUDIO_SRC_MP3;
    }
    return AUDIO_SRC_UNKNOWN;
}

/* ========== WAV 内部实现 ========== */

static esp_err_t wav_open_internal(wav_decoder_t *d, const char *filepath)
{
    FILE *f = d->file;
    uint8_t header[12];
    if (fread(header, 1, 12, f) != 12) {
        ESP_LOGE(TAG, "Failed to read RIFF header");
        return ESP_FAIL;
    }
    if (!chunk_match(header, RIFF_ID) || !chunk_match(header + 8, WAVE_ID)) {
        ESP_LOGE(TAG, "Not a RIFF/WAVE file");
        return ESP_FAIL;
    }

    bool fmt_found = false, data_found = false;
    uint8_t chunk_hdr[8];
    while (1) {
        if (fread(chunk_hdr, 1, 8, f) != 8) break;
        uint32_t chunk_size = read_le32(chunk_hdr + 4);

        if (chunk_match(chunk_hdr, FMT_ID)) {
            uint8_t fmt_buf[40];
            uint32_t fmt_read = (chunk_size > 40) ? 40 : chunk_size;
            if (fread(fmt_buf, 1, fmt_read, f) != fmt_read) return ESP_FAIL;

            d->format.format_tag      = read_le16(fmt_buf);
            d->format.channels        = read_le16(fmt_buf + 2);
            d->format.sample_rate     = read_le32(fmt_buf + 4);
            d->format.byte_rate       = read_le32(fmt_buf + 8);
            d->format.block_align     = read_le16(fmt_buf + 12);
            d->format.bits_per_sample = read_le16(fmt_buf + 14);

            if (d->format.format_tag != 0x0001) {
                ESP_LOGE(TAG, "Unsupported WAV format tag: 0x%04X", d->format.format_tag);
                return ESP_FAIL;
            }
            if (d->format.channels != 1 && d->format.channels != 2) return ESP_FAIL;
            if (d->format.bits_per_sample != 8 && d->format.bits_per_sample != 16) return ESP_FAIL;
            if (d->format.sample_rate < 8000 || d->format.sample_rate > 48000) return ESP_FAIL;

            d->is_8bit = (d->format.bits_per_sample == 8);
            fmt_found = true;
            if (chunk_size > fmt_read) fseek(f, chunk_size - fmt_read, SEEK_CUR);

        } else if (chunk_match(chunk_hdr, DATA_ID)) {
            d->format.data_size = chunk_size;
            d->format.data_offset = ftell(f);
            d->bytes_read = 0;
            if (d->format.byte_rate > 0)
                d->format.duration_ms = (uint32_t)((uint64_t)chunk_size * 1000 / d->format.byte_rate);
            data_found = true;
            break;
        } else {
            if (chunk_size > 0) fseek(f, chunk_size, SEEK_CUR);
            if (chunk_size & 1) fseek(f, 1, SEEK_CUR);
        }
    }
    if (!fmt_found || !data_found) return ESP_FAIL;

    ESP_LOGI(TAG, "WAV opened: %lu Hz, %d bit, %d ch, %lu ms",
             (unsigned long)d->format.sample_rate, d->format.bits_per_sample,
             d->format.channels, (unsigned long)d->format.duration_ms);
    return ESP_OK;
}

static esp_err_t wav_read_internal(wav_decoder_t *d, void *buffer,
                                   size_t buffer_size, size_t *bytes_read)
{
    uint32_t remaining = d->format.data_size - d->bytes_read;
    if (remaining == 0) { d->state = WAV_DECODER_EOF; return ESP_ERR_INVALID_STATE; }

    if (d->is_8bit) {
        size_t max_samples = buffer_size / 2;
        if (max_samples > remaining) max_samples = remaining;
        uint8_t *u8 = (uint8_t *)heap_caps_malloc(max_samples, MALLOC_CAP_SPIRAM);
        if (!u8) return ESP_ERR_NO_MEM;
        size_t actual = fread(u8, 1, max_samples, d->file);
        if (actual == 0) { heap_caps_free(u8); d->state = WAV_DECODER_EOF; return ESP_ERR_INVALID_STATE; }
        int16_t *s16 = (int16_t *)buffer;
        for (size_t i = 0; i < actual; i++) s16[i] = (int16_t)(((int)u8[i] - 128) << 8);
        *bytes_read = actual * 2;
        d->bytes_read += actual;
        heap_caps_free(u8);
    } else {
        size_t to_read = buffer_size;
        if (to_read > remaining) to_read = remaining;
        to_read &= ~1;
        if (to_read == 0) { d->state = WAV_DECODER_EOF; return ESP_ERR_INVALID_STATE; }
        size_t actual = fread(buffer, 1, to_read, d->file);
        if (actual == 0) { d->state = WAV_DECODER_EOF; return ESP_ERR_INVALID_STATE; }
        *bytes_read = actual;
        d->bytes_read += actual;
    }
    if (d->bytes_read >= d->format.data_size) d->state = WAV_DECODER_EOF;
    return ESP_OK;
}

/* ========== MP3 内部实现 ========== */

static esp_err_t mp3_open_internal(wav_decoder_t *d, const char *filepath)
{
    FILE *f = d->file;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 0) return ESP_FAIL;
    /* 整个文件载入 PSRAM */
    uint8_t *raw = (uint8_t *)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) { ESP_LOGE(TAG, "MP3 buffer alloc failed (%u)", (unsigned)size); return ESP_ERR_NO_MEM; }
    rewind(f);
    size_t got = fread(raw, 1, (size_t)size, f);
    if (got != (size_t)size) {
        ESP_LOGE(TAG, "MP3 read short: %u/%u", (unsigned)got, (unsigned)size);
        heap_caps_free(raw);
        return ESP_FAIL;
    }

    /* 【关键修复】剥离 ID3v2 标签。minimp3 底层 API 不会跳过 ID3v2，
     * 从 'ID3' 字节开始解会把标签当帧同步，输出全零样本（表现为播放电流声/静音）。
     * ID3v2 头 10 字节，size 用 syncsafe 编码（每字节 7 位）。 */
    size_t skip = 0;
    if (size >= 10 && raw[0] == 'I' && raw[1] == 'D' && raw[2] == '3') {
        size_t tag_size = ((size_t)(raw[6] & 0x7f) << 21) |
                          ((size_t)(raw[7] & 0x7f) << 14) |
                          ((size_t)(raw[8] & 0x7f) << 7) |
                          ((size_t)(raw[9] & 0x7f));
        skip = 10 + tag_size;
        if (raw[5] & 0x10) skip += 10;              /* footer 标志 */
        if (skip >= (size_t)size) skip = 0;          /* 异常则不过滤 */
        ESP_LOGI(TAG, "MP3 ID3v2 tag skipped: %u bytes", (unsigned)skip);
    }
    uint8_t *buf = raw + skip;
    d->mp3_file_size = (size_t)size - skip;
    /* 保留 raw 基址用于释放；mp3_file_buf 指向去标签后的音频流起点 */
    d->mp3_raw_buf = raw;
    d->mp3_file_buf = buf;
    fclose(f);            /* 已全量载入，文件句柄不再需要 */
    d->file = NULL;

    /* 解码器与帧 PCM 缓冲放 PSRAM */
    mp3dec_t *dec = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec || !pcm) {
        if (dec) heap_caps_free(dec);
        if (pcm) heap_caps_free(pcm);
        heap_caps_free(raw);   /* raw 是分配基址；buf=raw+skip 不能直接 free */
        d->mp3_raw_buf = NULL;
        d->mp3_file_buf = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(dec, 0, sizeof(mp3dec_t));
    mp3dec_init(dec);
    d->mp3_dec = dec;
    d->mp3_pcm_buf = pcm;
    d->mp3_pcm_valid = 0;
    d->mp3_pcm_pos = 0;

    /* 解第一帧以获取采样率/声道/码率 */
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(dec, buf, (int)d->mp3_file_size, pcm, &info);
    /* 诊断：首帧探测解码后的 PCM 峰值。若此值正常(~20000)而播放静音，
     * 说明解码器本身可用、问题在 read 循环的状态/offset；若此值也≈0，
     * 说明 minimp3 在此平台/编译选项下解码失效。 */
    {
        int fp = 0;
        int n = samples * (info.channels > 0 ? info.channels : 1);
        if (n > (int)(MINIMP3_MAX_SAMPLES_PER_FRAME)) n = MINIMP3_MAX_SAMPLES_PER_FRAME;
        for (int i = 0; i < n; i++) {
            int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > fp) fp = a;
        }
        d->probe_first_peak = fp;
        for (int i = 0; i < 8; i++) d->first_samples[i] = pcm[i];
    }
    if (info.hz == 0 || info.channels == 0 || samples <= 0) {
        ESP_LOGE(TAG, "MP3 first frame invalid (hz=%d ch=%d samples=%d)", info.hz, info.channels, samples);
        return ESP_FAIL;
    }
    /* 【关键修复】首帧仅用于探测 format，不能把解码器复位后跳过它。
     * MP3 Layer3 使用 bit reservoir，后续帧解码依赖前帧的 reservoir 状态；
     * 原实现首次 decode 后 mp3dec_init() 复位并把 offset 设为 frame_bytes，
     * 导致第二帧起 reservoir 丢失，PCM 逐帧错误 → 播放电流声/噪声。
     * 正确做法：保存首帧解析出的 format，然后重新 init，从 offset=0
     * 开始标准流式解码，每帧的 reservoir 都由解码器连续维护。 */
    d->format.format_tag = 0;
    d->format.channels = (uint16_t)info.channels;
    d->format.sample_rate = (uint32_t)info.hz;
    d->format.bits_per_sample = 16;
    d->format.block_align = info.channels * 2;
    /* PCM 字节率用于进度条（16-bit PCM） */
    d->format.byte_rate = info.hz * info.channels * 2;
    /* 时长估算：CBR 用码率，VBR 用平均字节率回退 */
    if (info.bitrate_kbps > 0)
        d->format.duration_ms = (uint32_t)((uint64_t)d->mp3_file_size * 8 / info.bitrate_kbps);
    else
        d->format.duration_ms = 0;

    /* 复位解码器，从文件头第 0 帧开始正式流式解码（丢弃用于探测的那一帧）。
     * 首帧多为 Xing/静音帧，丢弃不影响听感；关键是让 reservoir 从第一帧
     * 连续累积，后续每帧才能正确还原。 */
    mp3dec_init(dec);
    d->mp3_offset = 0;
    d->mp3_pcm_valid = 0;
    d->mp3_pcm_pos = 0;

    ESP_LOGI(TAG, "MP3 opened: %lu Hz, %d ch, %d kbps, %u bytes, ~%lu ms",
             (unsigned long)info.hz, info.channels, info.bitrate_kbps,
             (unsigned)d->mp3_file_size, (unsigned long)d->format.duration_ms);
    return ESP_OK;
}

static esp_err_t mp3_read_internal(wav_decoder_t *d, void *buffer,
                                   size_t buffer_size, size_t *bytes_read)
{
    int16_t *out = (int16_t *)buffer;
    size_t max_samples = buffer_size / 2;   /* 16-bit 样本数（含声道交错） */
    size_t produced = 0;

    while (produced < max_samples) {
        /* 先消费缓存的帧 PCM */
        while (d->mp3_pcm_pos < d->mp3_pcm_valid && produced < max_samples) {
            out[produced++] = d->mp3_pcm_buf[d->mp3_pcm_pos++];
        }
        if (produced >= max_samples) break;

        /* 帧 PCM 耗尽，解下一帧 */
        if (d->mp3_offset >= d->mp3_file_size) {
            d->state = WAV_DECODER_EOF;
            break;
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame((mp3dec_t *)d->mp3_dec,
                                          d->mp3_file_buf + d->mp3_offset,
                                          (int)(d->mp3_file_size - d->mp3_offset),
                                          d->mp3_pcm_buf, &info);
        if (info.frame_bytes > 0)
            d->mp3_offset += (size_t)info.frame_bytes;
        else
            d->mp3_offset += 1;   /* 跳过无法识别的字节，防止死循环 */

        if (samples > 0 && info.channels > 0) {
            d->mp3_pcm_valid = (size_t)samples * info.channels;
            d->mp3_pcm_pos = 0;
            /* 声道/采样率若中途变化则更新 */
            if (info.hz > 0) d->format.sample_rate = (uint32_t)info.hz;
            if (info.channels > 0 && info.channels != d->format.channels) {
                d->format.channels = (uint16_t)info.channels;
                d->format.byte_rate = info.hz * info.channels * 2;
                d->format.block_align = info.channels * 2;
            }
        } else {
            d->mp3_pcm_valid = 0;
            d->mp3_pcm_pos = 0;
        }
    }

    *bytes_read = produced * 2;
    d->bytes_read += *bytes_read;
    if (*bytes_read == 0 && d->state != WAV_DECODER_EOF) {
        d->state = WAV_DECODER_EOF;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void mp3_reset_decode(wav_decoder_t *d)
{
    mp3dec_init((mp3dec_t *)d->mp3_dec);
    d->mp3_offset = 0;
    d->mp3_pcm_valid = 0;
    d->mp3_pcm_pos = 0;
    d->bytes_read = 0;
}

/* ========== 公开 API（分派层） ========== */

esp_err_t wav_decoder_init(wav_decoder_t *decoder)
{
    if (!decoder) return ESP_ERR_INVALID_ARG;
    memset(decoder, 0, sizeof(wav_decoder_t));
    decoder->state = WAV_DECODER_IDLE;
    decoder->src = AUDIO_SRC_UNKNOWN;
    return ESP_OK;
}

esp_err_t wav_decoder_open(wav_decoder_t *decoder, const char *filepath)
{
    if (!decoder || !filepath) return ESP_ERR_INVALID_ARG;
    if (decoder->state != WAV_DECODER_IDLE) wav_decoder_close(decoder);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    audio_source_t src = detect_source(f);
    decoder->src = src;
    esp_err_t ret;
    if (src == AUDIO_SRC_WAV) {
        decoder->file = f;
        ret = wav_open_internal(decoder, filepath);
    } else if (src == AUDIO_SRC_MP3) {
        decoder->file = f;
        ret = mp3_open_internal(decoder, filepath);  /* 内部会 fclose(f) */
    } else {
        ESP_LOGE(TAG, "Unsupported audio format: %s", filepath);
        fclose(f);
        decoder->src = AUDIO_SRC_UNKNOWN;
        return ESP_FAIL;
    }

    if (ret != ESP_OK) {
        wav_decoder_close(decoder);
        return ret;
    }
    decoder->state = WAV_DECODER_READY;
    return ESP_OK;
}

esp_err_t wav_decoder_read(wav_decoder_t *decoder, void *buffer,
                           size_t buffer_size, size_t *bytes_read)
{
    if (!decoder || !buffer || !bytes_read) return ESP_ERR_INVALID_ARG;
    *bytes_read = 0;
    if (decoder->state == WAV_DECODER_EOF || decoder->state != WAV_DECODER_READY)
        return ESP_ERR_INVALID_STATE;

    esp_err_t ret;
    if (decoder->src == AUDIO_SRC_MP3)
        ret = mp3_read_internal(decoder, buffer, buffer_size, bytes_read);
    else if (decoder->src == AUDIO_SRC_WAV && decoder->file)
        ret = wav_read_internal(decoder, buffer, buffer_size, bytes_read);
    else
        return ESP_ERR_INVALID_STATE;

    /* 诊断探针：记录首个"非静音"样本块的前 16 个样本。
     * MP3 开头常有 Xing/encoder delay 静音帧（全 0），需跳过才能取到真实音频，
     * 用于通过 UART 对比实机解码 PCM 与离线 ffmpeg 结果是否一致。 */
    if (decoder->probe_count == 0 && *bytes_read >= 32) {
        const int16_t *src = (const int16_t *)buffer;
        size_t avail = *bytes_read / 2;
        /* 从块内"第一个非 0 样本"开始取 16 个，跳过 encoder delay 静音，
         * 这样能看到连续波形而非全 0 前缀。 */
        int16_t peak = 0;
        for (size_t i = 0; i < avail; i++) {
            int16_t a = src[i] < 0 ? (int16_t)(-src[i]) : src[i];
            if (a > peak) peak = a;
            if (src[i] != 0 && src[i] != -32768) {
                size_t n = (avail - i) < 16 ? (avail - i) : 16;
                memcpy(decoder->probe, src + i, n * sizeof(int16_t));
                decoder->probe_count = n;
                decoder->probe_offset = i;
                break;
            }
        }
        decoder->probe_peak = peak;
    }
    /* 全程峰值统计：每次 read 都更新，避免首帧 encoder delay 静音误导诊断。
     * probe_last_peak 反映真正送往 audio_output_write 的数据幅度。 */
    {
        const int16_t *src = (const int16_t *)buffer;
        size_t avail = *bytes_read / 2;
        int pk = 0;
        for (size_t i = 0; i < avail; i++) {
            int32_t v = src[i];
            int a = v < 0 ? -v : v;
            if (a > pk) pk = a;
        }
        decoder->probe_last_peak = pk;
        if (pk > decoder->probe_max_peak) decoder->probe_max_peak = pk;
        decoder->probe_reads++;
    }
    return ret;
}

esp_err_t wav_decoder_seek(wav_decoder_t *decoder, uint32_t position_ms)
{
    if (!decoder || decoder->state == WAV_DECODER_IDLE) return ESP_ERR_INVALID_STATE;

    if (decoder->src == AUDIO_SRC_MP3) {
        /* MP3 随机 seek 代价高：从头解码到目标附近。
         * 估算每帧时长 = 1152/sample_rate，目标帧号 = position_ms/帧时长。 */
        mp3_reset_decode(decoder);
        if (position_ms > 0 && decoder->format.sample_rate > 0 && decoder->mp3_file_size > 0) {
            /* 按字节比例粗定位（CBR 近似），并对齐到帧边界由解码器自行同步 */
            uint64_t target = (uint64_t)position_ms * decoder->mp3_file_size /
                              (decoder->format.duration_ms ? decoder->format.duration_ms : 1);
            if (target >= decoder->mp3_file_size) target = decoder->mp3_file_size - 1;
            decoder->mp3_offset = (size_t)target;
            /* 从目标点解码可能需要参考前帧，简单回退 4KB 给解码器同步 */
            if (decoder->mp3_offset > 4096) decoder->mp3_offset -= 4096;
            else decoder->mp3_offset = 0;
            mp3dec_init((mp3dec_t *)decoder->mp3_dec);
        }
        decoder->state = WAV_DECODER_READY;
        return ESP_OK;
    }

    /* WAV seek */
    if (!decoder->file) return ESP_ERR_INVALID_STATE;
    if (position_ms > decoder->format.duration_ms) position_ms = decoder->format.duration_ms;
    uint64_t byte_offset = (uint64_t)position_ms * decoder->format.byte_rate / 1000;
    if (decoder->format.block_align > 0)
        byte_offset = (byte_offset / decoder->format.block_align) * decoder->format.block_align;
    if (byte_offset > decoder->format.data_size) byte_offset = decoder->format.data_size;
    long pos = (long)(decoder->format.data_offset + byte_offset);
    if (fseek(decoder->file, pos, SEEK_SET) != 0) return ESP_FAIL;
    decoder->bytes_read = (uint32_t)byte_offset;
    decoder->state = WAV_DECODER_READY;
    return ESP_OK;
}

esp_err_t wav_decoder_close(wav_decoder_t *decoder)
{
    if (!decoder) return ESP_ERR_INVALID_ARG;
    if (decoder->file) { fclose(decoder->file); decoder->file = NULL; }

    if (decoder->mp3_dec) { heap_caps_free(decoder->mp3_dec); decoder->mp3_dec = NULL; }
    if (decoder->mp3_pcm_buf) { heap_caps_free(decoder->mp3_pcm_buf); decoder->mp3_pcm_buf = NULL; }
    if (decoder->mp3_raw_buf) { heap_caps_free(decoder->mp3_raw_buf); decoder->mp3_raw_buf = NULL; }
    decoder->mp3_file_buf = NULL;

    decoder->state = WAV_DECODER_IDLE;
    decoder->src = AUDIO_SRC_UNKNOWN;
    memset(&decoder->format, 0, sizeof(wav_format_t));
    decoder->bytes_read = 0;
    decoder->is_8bit = false;
    decoder->mp3_file_size = 0;
    decoder->mp3_offset = 0;
    decoder->mp3_pcm_valid = 0;
    decoder->mp3_pcm_pos = 0;
    return ESP_OK;
}

const wav_format_t* wav_decoder_get_format(wav_decoder_t *decoder)
{
    if (!decoder || decoder->state == WAV_DECODER_IDLE) return NULL;
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
    const char *ext = strrchr(filepath, '.');
    if (!ext || (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".wave") != 0)) return false;
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    uint8_t header[12];
    bool ok = (fread(header, 1, 12, f) == 12) &&
              chunk_match(header, RIFF_ID) && chunk_match(header + 8, WAVE_ID);
    fclose(f);
    return ok;
}

void audio_decoder_get_mp3_table_probe(int out[4])
{
    if (out) mp3dec_table_probe(out);
}

void audio_decoder_get_mp3_deep_probe(int out[8])
{
    if (out) mp3dec_deep_probe(out);
}

bool audio_decoder_is_supported(const char *filepath)
{
    if (!filepath) return false;
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    audio_source_t src = detect_source(f);
    fclose(f);
    return src == AUDIO_SRC_WAV || src == AUDIO_SRC_MP3;
}
