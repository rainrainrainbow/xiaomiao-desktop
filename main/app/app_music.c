/**
 * @file app_music.c
 * @brief 音乐应用 - 三页面架构（列表/播放/设置）
 *
 * 功能：
 * - 列表页：优先扫描内置 FAT（/flash/music），回退存储卡（/sdcard/music）；
 *           支持 .wav / .mp3（minimp3 解码）
 * - 播放页：歌词显示(LRC)、频谱可视化、播放进度(触摸拖动/键盘调节)、
 *           音量、循环模式(含顺序播放)、上下一首、暂停、真 EQ 音效
 * - 设置页：音量、频谱开关、循环模式、播放输出设备、音效预设
 * - 后台播放：切到桌面/其它 App 不停播，注册到 bg_tasks 供"最近任务"查看
 *
 * 操作说明：
 *   列表页：UP/DOWN选择  A播放  RIGHT进入播放页  LEFT进入设置页  B返回
 *   播放页：A进入/确认进度调节  LEFT/RIGHT切歌  UP/DOWN播放暂停  B返回列表
 *   进度调节模式：LEFT/RIGHT±5秒  UP/DOWN±30秒  A确认跳转  B取消
 *   设置页：UP/DOWN选择  A切换  LEFT/RIGHT调节  B返回
 *
 * 架构：
 * - 使用 drv_audio_decoder 解码WAV文件
 * - 使用 drv_audio_output 播放PCM数据（通过I2S DAC或蓝牙A2DP）
 * - 使用 drv_lrc 解析LRC歌词文件
 * - 独立播放任务避免阻塞UI
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "app_sys_api.h"
#include "bg_tasks.h"
#include "ui_framework.h"
#include "driver/drv_audio_output.h"
#include "driver/drv_audio_decoder.h"
#include "driver/drv_lrc.h"
#include "system/sys_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>      /* EQ 系数计算：expf（仅在配置变化时调用，非逐样本） */

static const char *TAG = "APP_MUSIC";

#define MUSIC_MAX_ENTRIES 32
#define MUSIC_PATH_LEN    128  /* 文件名长度 */
#define MUSIC_FULLPATH_LEN 256 /* 完整路径长度 */
#define MUSIC_DISPLAY_LEN 32   /* 显示名称长度 */
/* 解码缓冲：4096 样本(8KB) = 44.1k 立体声约 46ms/块。
 * 块越大，播放循环每秒迭代次数越少（512样本时约170次/秒→4096时约21次/秒），
 * 每次的锁/分配/I2S写入开销摊薄，且 SD 卡补窗（毫秒级停顿）期间
 * I2S DMA 余量更从容，显著缓解 TF 卡播放卡顿。缓冲在 PSRAM。 */
#define DECODE_BUF_SIZE   4096  /* 解码缓冲区大小（16位样本数） */

/* ========== 播放模式 ========== */
typedef enum {
    PLAY_MODE_SINGLE = 0,      /* 单曲循环（播完重播当前） */
    PLAY_MODE_LIST_LOOP,       /* 列表循环 */
    PLAY_MODE_RANDOM,          /* 随机播放 */
    PLAY_MODE_ORDER,           /* 顺序播放：列表播完停止，不循环 */
    PLAY_MODE_MAX
} play_mode_t;

/* ========== EQ 音效预设 ==========
 * APP_CFG_MUSIC_EQ 严格按需求存 0=OFF/1=POP/2=ROCK/3=CLASSICAL/4=BASS。
 * 旧版本该键存的是频谱开关(0/1)，首次升级时迁移到独立 NVS 键。 */
typedef enum {
    MUSIC_EQ_OFF = 0,          /* 关闭音效（PCM 直通） */
    MUSIC_EQ_POP,              /* 流行 */
    MUSIC_EQ_ROCK,             /* 摇滚 */
    MUSIC_EQ_CLASSICAL,        /* 古典 */
    MUSIC_EQ_BASS,             /* 重低音 */
    MUSIC_EQ_MAX
} music_eq_preset_t;

/* 频谱动画开关单独存：原 APP_CFG_MUSIC_EQ 已被 EQ 预设占用，不引入新配置文件 */
#define MUSIC_NVS_NS          "music_ui"
#define MUSIC_NVS_KEY_SPEC    "spectrum"

/* ========== 页面类型 ========== */
typedef enum {
    MUSIC_PAGE_LIST = 0,   /* 列表页 */
    MUSIC_PAGE_PLAY,       /* 播放页 */
    MUSIC_PAGE_SETTINGS,   /* 设置页 */
    MUSIC_PAGE_MAX
} music_page_t;

/* ========== UI布局常量 ========== */
#define MUSIC_HEADER_H     18   /* 顶部状态区高度 */
#define MUSIC_PROGRESS_H   6    /* 进度条高度 */
#define MUSIC_EQ_H         14   /* 均衡器可视化区高度 */
#define MUSIC_DIVIDER_H    2    /* 分隔线高度 */
#define MUSIC_LIST_TOP     (MUSIC_HEADER_H + MUSIC_PROGRESS_H + 2 + MUSIC_EQ_H + 2 + MUSIC_DIVIDER_H)
#define MUSIC_LRC_LINES    4    /* 播放页歌词行数 */

/* ========== UI对象变量 ========== */
static lv_obj_t *s_music_obj = NULL;
static lv_obj_t *s_progress_bar = NULL;   /* 播放进度条 */
static lv_obj_t *s_eq_bars[4] = {NULL};   /* 4段均衡器条 */
static lv_obj_t *s_status_lbl = NULL;     /* 状态文字 */
static lv_obj_t *s_vol_lbl = NULL;        /* 音量标签 */
static lv_obj_t *s_lrc_labels[MUSIC_LRC_LINES] = {NULL}; /* 歌词标签 */
static lv_obj_t *s_time_lbl = NULL;       /* 播放时间标签 */
static lv_timer_t *s_ui_timer = NULL;     /* UI定时器，200ms刷新 */

/* ========== 页面状态 ========== */
static music_page_t s_music_page = MUSIC_PAGE_LIST;
static int s_music_sel = 0;               /* 当前选中索引 */
static int s_music_scroll = 0;            /* 列表滚动偏移 */
static int s_music_vis_rows = 0;          /* 可见行数 */
static int s_music_row_h = 15;

/* 设置页选中项 */
static int s_settings_sel = 0;
#define SETTINGS_ITEM_COUNT 7  /* 音量/频谱/循环/输出/音效/当前曲/清空队列 */

/* 文件列表 */
/* 纯 UI 音乐列表缓存，放 PSRAM：4KB 内部 DRAM 释放。
 * 每个条目保存完整路径（/flash/music/xxx.mp3 或 /sdcard/music/yyy.mp3），
 * 以便同时列出内置 FAT 与 SD 卡两个目录的音频。 */
EXT_RAM_BSS_ATTR static char s_music_entries[MUSIC_MAX_ENTRIES][MUSIC_FULLPATH_LEN];
static int s_music_count = 0;
/* 音频搜索目录：内置 5MB FAT（/flash/music）与存储卡（/sdcard/music）。
 * 两个目录都会被扫描并合并到同一列表，插卡后无需手动切换。 */
#define MUSIC_DIR_FLASH  "/flash/music"
#define MUSIC_DIR_SD     "/sdcard/music"
/* 兼容测试探针：保留最近一次扫描的主目录字段（取实际使用的第一个目录） */
static char s_music_current_path[MUSIC_PATH_LEN] = MUSIC_DIR_FLASH;

/* 播放状态 */
static volatile bool s_is_playing = false;
static volatile bool s_is_paused = false;
static volatile int s_playing_idx = -1;         /* 正在播放的文件索引 */
static int s_volume = 50;
static play_mode_t s_play_mode = PLAY_MODE_SINGLE;
static bool s_eq_enabled = true;       /* 频谱显示开关 */
static bool s_lrc_enabled = true;      /* 歌词显示开关 */

/* 解码器和歌词 */
static wav_decoder_t s_wav_decoder;
static lrc_parser_t s_lrc;
static TaskHandle_t s_play_task = NULL;

/* ---- 后台播放 / 进度调节：跨 UI 主线程与播放任务的共享状态 ----
 * s_is_playing / s_is_paused / s_playing_idx / s_seek_req_ms 均为 volatile，
 * UI 只写请求标志，真正的解码器 seek 由播放任务在自身上下文完成。 */
static int s_bg_task_id = -1;                       /* bg_tasks 槽位，-1=未注册 */
static volatile int32_t s_seek_req_ms = -1;         /* UI 提交的 seek 请求(ms)，-1=无 */
static volatile bool s_seek_mode = false;           /* 播放页：键盘进度调节模式 */
static uint32_t s_seek_cursor_ms = 0;               /* 进度调节游标(ms)，UI 线程独占 */
static music_eq_preset_t s_eq_preset = MUSIC_EQ_OFF; /* 当前音效预设 */
static volatile bool s_eq_reload = false;           /* UI 改预设后置位，由播放任务重载系数 */

/* 拼接完整路径：path + '/' + name（定义见下方，此处前置声明供扫描使用） */
static void music_build_path(const char *path, const char *name, char *out, size_t out_size);

/* ========== EQ 音效：定点一阶 IIR 低通 + 高通互补拆分 ==========
 * 思路：x 经过一阶低通得到低频 lp，高频 hp = x - lp；
 *      输出 = lp * 低频增益 + hp * 高频增益（Q14 定点增益）。
 * 每声道只保留 1 个 int32 历史值，逐样本无浮点运算，ESP32 上开销极小。
 * 滤波状态为全局 static（BSS），不占播放任务栈；切歌/seek 时清零防止爆音。 */
typedef struct {
    int32_t lp_state[2];    /* 左右声道低通历史值 */
    int16_t a_lp_q15;       /* 低通系数 a=1-exp(-2πfc/fs)，Q15 */
    int16_t g_bass_q14;     /* 低频增益 Q14（16384=1.0） */
    int16_t g_treble_q14;   /* 高频增益 Q14 */
    uint8_t channels;
    bool enabled;
} music_eq_state_t;

static music_eq_state_t s_eq_state;

static void music_eq_reset_state(void)
{
    s_eq_state.lp_state[0] = 0;
    s_eq_state.lp_state[1] = 0;
}

/* 预设 + 采样率 -> 系数。仅在开播/切歌/改预设时调用，非逐样本路径 */
static void music_eq_setup(music_eq_preset_t preset, uint32_t sample_rate, uint8_t channels)
{
    /* fc / 低频增益Q14 / 高频增益Q14；增益预留 0.85~0.9 余量防削波 */
    static const struct {
        uint16_t fc;
        int16_t bass_q14;
        int16_t treble_q14;
    } cfg[MUSIC_EQ_MAX] = {
        {   0, 16384, 16384},  /* OFF：旁路，不走滤波 */
        { 350, 14746, 16958},  /* POP：轻微提亮高频 (0.90 / 1.035) */
        { 260, 17408, 17408},  /* ROCK：两端抬升 V 型 (1.0625 / 1.0625) */
        { 500, 14788, 16344},  /* CLASSICAL：平直略透 (0.9025 / 0.9975) */
        { 120, 20972, 11141},  /* BASS：强低频弱高频 (1.28 / 0.68) */
    };

    memset(&s_eq_state, 0, sizeof(s_eq_state));
    s_eq_state.channels = channels ? channels : 2;
    if (preset <= MUSIC_EQ_OFF || preset >= MUSIC_EQ_MAX || sample_rate == 0) {
        s_eq_state.enabled = false;   /* OFF：直接旁路，零 CPU 开销 */
        return;
    }
    s_eq_state.enabled = true;
    s_eq_state.g_bass_q14 = cfg[preset].bass_q14;
    s_eq_state.g_treble_q14 = cfg[preset].treble_q14;

    /* a = 1 - exp(-2πfc/fs)，只在配置变化时算一次浮点 */
    float a = 1.0f - expf(-2.0f * 3.14159265f * (float)cfg[preset].fc / (float)sample_rate);
    int a_q = (int)(a * 32768.0f);
    if (a_q < 1) a_q = 1;
    if (a_q > 32767) a_q = 32767;
    s_eq_state.a_lp_q15 = (int16_t)a_q;
    music_eq_reset_state();
    ESP_LOGI(TAG, "EQ preset=%d fc=%u a_q15=%d fs=%u ch=%u",
             (int)preset, (unsigned)cfg[preset].fc, a_q,
             (unsigned)sample_rate, (unsigned)s_eq_state.channels);
}

/* 对 16bit PCM 就地滤波（左右声道交错）。逐样本只有整数乘加/移位。 */
static void music_eq_apply(int16_t *pcm, size_t bytes)
{
    if (!s_eq_state.enabled || !pcm) return;
    size_t n = bytes / 2;
    int32_t a = s_eq_state.a_lp_q15;
    int ch = (s_eq_state.channels > 1) ? 2 : 1;

    for (size_t i = 0; i < n; i++) {
        int c = (ch == 2) ? (i & 1) : 0;
        int32_t x = pcm[i];
        int32_t lp = s_eq_state.lp_state[c];
        /* y += a*(x-y)：Q15 乘，int64 中间量防止 32767*65535 溢出 */
        lp += (int32_t)(((int64_t)a * ((int64_t)x - lp)) >> 15);
        s_eq_state.lp_state[c] = lp;
        int32_t hp = x - lp;   /* 互补高通 */
        int64_t y = ((int64_t)lp * s_eq_state.g_bass_q14 +
                     (int64_t)hp * s_eq_state.g_treble_q14 + 8192) >> 14;
        if (y > 32767) y = 32767;
        else if (y < -32768) y = -32768;
        pcm[i] = (int16_t)y;
    }
}

/* 频谱动画开关存到本文件专属 NVS 键（不动 app_builtin.h / app_sys_api.h） */
static void music_save_spectrum(void)
{
    nvs_handle_t h;
    if (nvs_open(MUSIC_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, MUSIC_NVS_KEY_SPEC, s_eq_enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* EQ 预设仍走统一配置门面：严格 0..4 */
static void music_save_eq_cfg(void)
{
    app_sys_cfg_set_int(APP_CFG_MUSIC_EQ, (int)s_eq_preset);
}

static void music_load_eq_cfg(void)
{
    /* 1) 读取频谱开关；NVS 键不存在说明是旧版本数据 */
    nvs_handle_t h;
    int32_t spec = 1;
    bool legacy = true;
    if (nvs_open(MUSIC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        legacy = (nvs_get_i32(h, MUSIC_NVS_KEY_SPEC, &spec) != ESP_OK);
        nvs_close(h);
    }

    /* 2) 读取 EQ 预设 */
    int v = app_sys_cfg_get_int(APP_CFG_MUSIC_EQ, MUSIC_EQ_OFF);
    if (legacy) {
        /* 旧版本：APP_CFG_MUSIC_EQ 存的是频谱开关 0/1，迁移一次 */
        s_eq_enabled = (v != 0);
        s_eq_preset = MUSIC_EQ_OFF;
        music_save_spectrum();
        music_save_eq_cfg();
        ESP_LOGI(TAG, "Migrated legacy music EQ config: spectrum=%d", (int)s_eq_enabled);
    } else {
        s_eq_enabled = (spec != 0);
        s_eq_preset = (v >= 0 && v < MUSIC_EQ_MAX) ? (music_eq_preset_t)v : MUSIC_EQ_OFF;
    }
}

static void music_eq_set_preset(music_eq_preset_t preset)
{
    if (preset < 0 || preset >= MUSIC_EQ_MAX) return;
    s_eq_preset = preset;
    music_save_eq_cfg();
    /* 不在这里直接改 s_eq_state：UI 线程与播放任务会同时碰它。
     * 只置标志，由播放任务在自身上下文调用 music_eq_setup，避免撕裂状态/爆音。 */
    s_eq_reload = true;
}

/* ========== 文件扫描 ========== */
/* 统计指定目录中可解码音频文件数（不填充列表），用于目录自动选择 */
static int music_count_audio(const char *path)
{
    int count = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".wave") == 0 ||
            strcasecmp(ext, ".mp3") == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* 扫描一个目录，把其中的音频文件以【完整路径】追加到列表。
 * path 为 NULL 时忽略（例如 SD 卡未挂载）。返回本次新增数量。 */
static int music_scan_dir(const char *path)
{
    if (!path) return 0;

    int added = 0;
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open music directory: %s", path);
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_music_count < MUSIC_MAX_ENTRIES) {
        if (entry->d_name[0] == '.') continue;

        /* 支持 .wav / .mp3 / .wave */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".wave") != 0 &&
            strcasecmp(ext, ".mp3") != 0) continue;

        /* 直接保存完整路径：目录 + '/' + 文件名 */
        music_build_path(path, entry->d_name,
                         s_music_entries[s_music_count], MUSIC_FULLPATH_LEN);
        s_music_count++;
        added++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "Music scan: +%d audio files in %s", added, path);
    return added;
}

/* 自动搜索：同时扫描内置 FAT 与 SD 卡，合并到统一列表。
 * 先 Flash 后 SD，这样内置《人世间》始终在列表前部；插卡后卡内音频追加在后面。 */
static void music_scan_auto(void)
{
    s_music_count = 0;

    int n_flash = music_count_audio(MUSIC_DIR_FLASH);
    strncpy(s_music_current_path, MUSIC_DIR_FLASH, MUSIC_PATH_LEN - 1);
    s_music_current_path[MUSIC_PATH_LEN - 1] = '\0';
    music_scan_dir(MUSIC_DIR_FLASH);

    /* 若开机时未挂载（例如开机后才插卡），此处再尝试挂载一次 */
    if (!app_sys_sdcard_ready()) {
        app_sys_sdcard_try_mount();
    }

    int n_sd = 0;
    if (app_sys_sdcard_ready()) {
        n_sd = music_count_audio(MUSIC_DIR_SD);
        music_scan_dir(MUSIC_DIR_SD);
    }

    ESP_LOGI(TAG, "Music dir scan done: flash=%d sd=%d total=%d",
             n_flash, n_sd, s_music_count);
}

/* ========== 播放控制 ========== */

/* 根据播放模式计算下一个播放索引 */
static int music_next_index(int current)
{
    if (s_music_count <= 0) return -1;
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE:
        return current;  /* 单曲循环：重播当前 */
    case PLAY_MODE_LIST_LOOP:
        return (current + 1) % s_music_count;
    case PLAY_MODE_ORDER:
        /* 顺序播放：到列表末尾返回 -1，播放任务收尾停止，不循环 */
        return (current + 1 < s_music_count) ? (current + 1) : -1;
    case PLAY_MODE_RANDOM:
    default: {
        int next = rand() % s_music_count;
        if (next == current && s_music_count > 1)
            next = (next + 1) % s_music_count;
        return next;
    }
    }
}

static int music_prev_index(int current)
{
    if (s_music_count <= 0) return -1;
    return (current - 1 + s_music_count) % s_music_count;
}

/* 拼接完整路径：path + '/' + name，始终保证结果以 NUL 结尾且不越界。
 * 手动拼接而非 snprintf，避免 -Wformat-truncation（128+1+128=257 > 256）告警。 */
static void music_build_path(const char *path, const char *name, char *out, size_t out_size)
{
    if (out_size == 0) return;

    size_t pos = 0;
    size_t cap = out_size - 1;  /* 预留结尾 NUL */

    if (path) {
        size_t plen = strlen(path);
        if (plen > cap) plen = cap;
        memcpy(out, path, plen);
        pos = plen;
    }
    if (pos < cap) {
        out[pos++] = '/';
    }
    if (name && pos < cap) {
        size_t nlen = strlen(name);
        size_t remain = cap - pos;
        if (nlen > remain) nlen = remain;
        memcpy(out + pos, name, nlen);
        pos += nlen;
    }
    out[pos] = '\0';
}

/* 取第 idx 个音频条目的完整路径（条目本身已存完整路径） */
static void music_get_fullpath(int idx, char *out, size_t out_size)
{
    strncpy(out, s_music_entries[idx], out_size - 1);
    out[out_size - 1] = '\0';
}

/* 释放后台任务槽位（幂等）。播放任务所有退出路径统一调用。 */
static void music_bg_task_release(void)
{
    if (s_bg_task_id >= 0) {
        bg_tasks_remove(s_bg_task_id);
        s_bg_task_id = -1;
    }
}

/* ========== 播放任务 ========== */
static void music_play_task(void *arg)
{
    int file_idx = (int)(intptr_t)arg;
    char fullpath[MUSIC_FULLPATH_LEN];

    music_get_fullpath(file_idx, fullpath, sizeof(fullpath));

    ESP_LOGI(TAG, "Starting playback: %s", fullpath);

    /* 初始化并打开WAV解码器 */
    wav_decoder_init(&s_wav_decoder);
    if (wav_decoder_open(&s_wav_decoder, fullpath) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", fullpath);
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
        music_bg_task_release();
        vTaskDelete(NULL);
        return;
    }

    const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
    ESP_LOGI(TAG, "WAV: %lu Hz, %d bit, %d ch, %lu ms",
             (unsigned long)fmt->sample_rate, fmt->bits_per_sample,
             fmt->channels, (unsigned long)fmt->duration_ms);

    /* 打开音频输出 */
    esp_err_t ret = audio_output_open(fmt->sample_rate, 16, fmt->channels);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_output_open failed: %s", esp_err_to_name(ret));
        wav_decoder_close(&s_wav_decoder);
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
        music_bg_task_release();
        vTaskDelete(NULL);
        return;
    }

    /* EQ：按当前文件采样率/声道数计算滤波系数（OFF 预设自动旁路） */
    music_eq_setup(s_eq_preset, fmt->sample_rate, fmt->channels);

    /* 解码缓冲区（16位PCM样本） */
    /* 解码 PCM 缓冲通过 audio_output_write 送入 I2S，源数据可驻留 PSRAM；
       I2S DMA 描述符与硬件缓冲仍由驱动固定在内部 RAM。 */
    int16_t *decode_buf = heap_caps_malloc(DECODE_BUF_SIZE * sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decode_buf) {
        decode_buf = heap_caps_malloc(DECODE_BUF_SIZE * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    if (!decode_buf) {
        ESP_LOGE(TAG, "Failed to allocate decode buffer");
        audio_output_stop();
        wav_decoder_close(&s_wav_decoder);
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
        music_bg_task_release();
        vTaskDelete(NULL);
        return;
    }

    s_is_playing = true;
    s_is_paused = false;
    int progress_tick = 0;   /* 进度上报节流：约每 20 个解码块刷一次 */

    while (1) {
        /* UI 线程改了音效预设：在播放任务上下文重载系数，避免并发改写滤波状态 */
        if (s_eq_reload) {
            s_eq_reload = false;
            music_eq_setup(s_eq_preset, fmt->sample_rate, fmt->channels);
        }

        /* 处理 UI 线程提交的 seek 请求：
         * 先 stop 冲掉 I2S DMA 中旧数据（避免拖动瞬间爆音），
         * 再 seek（解码器内部已重置状态），最后清 EQ 历史并继续写入。 */
        if (s_seek_req_ms >= 0) {
            int32_t target_ms = s_seek_req_ms;
            s_seek_req_ms = -1;              /* 消费请求 */
            ESP_LOGI(TAG, "Seek request: %d ms", (int)target_ms);
            audio_output_stop();
            esp_err_t se = wav_decoder_seek(&s_wav_decoder, (uint32_t)target_ms);
            if (se != ESP_OK) ESP_LOGW(TAG, "wav_decoder_seek failed: %s", esp_err_to_name(se));
            music_eq_reset_state();          /* 清滤波历史，防止直流冲击 */
            if (s_bg_task_id >= 0) bg_tasks_set_progress(s_bg_task_id, 0, NULL);
        }

        /* 检查是否被要求停止 */
        if (!s_is_playing || s_playing_idx != file_idx) {
            break;
        }

        /* 暂停处理 */
        if (s_is_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 读取解码数据 */
        size_t bytes_read = 0;
        ret = wav_decoder_read(&s_wav_decoder, decode_buf,
                               DECODE_BUF_SIZE * sizeof(int16_t), &bytes_read);

        if (ret != ESP_OK || bytes_read == 0) {
            /* 文件播放完毕 */
            if (wav_decoder_get_state(&s_wav_decoder) == WAV_DECODER_EOF) {
                ESP_LOGI(TAG, "Playback finished: %s", s_music_entries[file_idx]);

                /* 根据播放模式决定下一步 */
                int next_idx = music_next_index(file_idx);
                if (next_idx < 0) break;

                file_idx = next_idx;
                s_playing_idx = file_idx;

                /* 关闭当前文件，打开下一个 */
                wav_decoder_close(&s_wav_decoder);

                music_get_fullpath(file_idx, fullpath, sizeof(fullpath));

                if (wav_decoder_open(&s_wav_decoder, fullpath) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open next file: %s", fullpath);
                    break;
                }

                const wav_format_t *next_fmt = wav_decoder_get_format(&s_wav_decoder);
                audio_output_open(next_fmt->sample_rate, 16, next_fmt->channels);
                /* 切歌：清 EQ 历史并按新文件格式重算系数 */
                music_eq_reset_state();
                music_eq_setup(s_eq_preset, next_fmt->sample_rate, next_fmt->channels);

                /* 加载歌词 */
                char lrc_base[MUSIC_FULLPATH_LEN];
                music_get_fullpath(file_idx, lrc_base, sizeof(lrc_base));
                char *dot = strrchr(lrc_base, '.');
                if (dot) *dot = '\0';
                lrc_parser_load(&s_lrc, lrc_base);
                continue;
            }
            break;
        }

        /* 真音效：PCM 送 I2S 之前做 EQ 滤波（OFF 预设内部直接旁路） */
        music_eq_apply(decode_buf, bytes_read);

        /* 写入音频输出（通过I2S DAC或蓝牙A2DP） */
        ret = audio_output_write(decode_buf, bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio_output_write returned %s", esp_err_to_name(ret));
        }

        /* 向后台任务表上报进度（播放任务内完成，不依赖 UI 定时器） */
        if ((++progress_tick % 20) == 0 && s_bg_task_id >= 0) {
            const wav_format_t *pf = wav_decoder_get_format(&s_wav_decoder);
            uint32_t pm = wav_decoder_get_position_ms(&s_wav_decoder);
            int pct = (pf && pf->duration_ms) ? (int)(pm * 100 / pf->duration_ms) : 0;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            bg_tasks_set_progress(s_bg_task_id, pct, NULL);
        }
    }

    /* 清理 */
    heap_caps_free(decode_buf);
    audio_output_stop();
    wav_decoder_close(&s_wav_decoder);
    /* 播放真正结束/被停止才移除后台任务；切界面(destroy)不会走到这里 */
    music_bg_task_release();

    s_is_playing = false;
    s_playing_idx = -1;
    /* 仅在句柄仍指向本任务时清除，避免异常超时下误清新任务句柄导致任务泄漏 */
    if (s_play_task == xTaskGetCurrentTaskHandle()) s_play_task = NULL;
    vTaskDelete(NULL);
}

/* 请求停止播放任务并等待其退出：
 * 播放任务在退出前会释放解码缓冲、停止音频输出、关闭解码器，最后把 s_play_task 置空。
 * 轮询等待句柄真正置空，比原来的固定 100ms 延时可靠，避免切歌/退出时新旧任务
 * 并发访问全局 s_wav_decoder 与 I2S。 */
static void music_stop_task_and_wait(void)
{
    s_is_playing = false;
    s_playing_idx = -1;
    s_is_paused = false;

    for (int i = 0; i < 50 && s_play_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_play_task != NULL) {
        /* 极少见：任务未及时退出。不强制 vTaskDelete（可能遗留 I2S 互斥锁导致死锁），
         * 仅告警；任务在下一次循环检测到停止标志后会自行清理并退出。 */
        ESP_LOGW(TAG, "music_play task exit is slow, will self-stop on next loop");
    }
}

static void music_start_playback(int idx)
{
    if (idx < 0 || idx >= s_music_count) return;

    /* 先停止并等待旧任务真正退出，避免并发操作 s_wav_decoder / I2S */
    music_stop_task_and_wait();

    s_playing_idx = idx;
    s_is_paused = false;

    /* 加载歌词（从文件路径去掉扩展名，找同名的.lrc） */
    lrc_parser_init(&s_lrc);
    {
        char lrc_base[MUSIC_FULLPATH_LEN];
        music_get_fullpath(idx, lrc_base, sizeof(lrc_base));
        char *dot = strrchr(lrc_base, '.');
        if (dot) *dot = '\0';
        lrc_parser_load(&s_lrc, lrc_base);
    }

    /* MP3 解码 scratch（~16KB）已通过 MINIMP3_PSRAM_SCRATCH 移到 PSRAM，
     * 不再占用任务栈。播放任务自身只需容纳普通调用链，8KB 足够。
     * 栈仍在内部 RAM（flash cache disabled 时需可访问）。 */
    #define MUSIC_PLAY_TASK_STACK 8192
    /* 音频是实时流：优先级必须高于 UI(5)/LED(5)，否则 LVGL 刷屏与 FAT
     * 解码抢占会让 I2S DMA 欠载，听感就是"电流声/沙沙声"而非音乐。 */
    BaseType_t rt = xTaskCreate(music_play_task, "music_play", MUSIC_PLAY_TASK_STACK,
                                (void*)(intptr_t)idx, 12, &s_play_task);
    if (rt != pdPASS) {
        /* 任务创建失败（内存不足）：复位播放状态，避免 UI 误判为正在播放 */
        ESP_LOGE(TAG, "xTaskCreate(music_play) failed");
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
        music_bg_task_release();
    } else {
        /* 登记后台任务：切到桌面/其它 App 后仍在"最近任务"中可见可控 */
        s_bg_task_id = bg_tasks_register(BG_TASK_MUSIC, lang_get(STR_MUSIC_BACKGROUND),
                                         NULL, NULL, NULL, NULL);
        if (s_bg_task_id >= 0) {
            bg_tasks_set_state(s_bg_task_id,
                               s_is_paused ? BG_TASK_STATE_PAUSED : BG_TASK_STATE_RUNNING);
        } else {
            ESP_LOGW(TAG, "bg_tasks_register(MUSIC) failed, no free slot");
        }
    }
}

static void music_stop_playback(void)
{
    music_stop_task_and_wait();
    /* 兜底：任务若尚未进入清理分支，这里确保槽位释放 */
    music_bg_task_release();
}

/* 对外查询：播放任务是否仍存活（含暂停态）。声明由集成方在头文件补充。 */
bool music_is_playing(void)
{
    return s_is_playing;
}

/* 切换到指定曲目 */
static void music_play_track(int idx)
{
    if (idx < 0 || idx >= s_music_count) return;
    s_music_sel = idx;
    music_start_playback(idx);
}

/* 上一首 */
static void music_play_prev(void)
{
    if (s_music_count <= 0) return;
    int idx = music_prev_index(s_playing_idx >= 0 ? s_playing_idx : s_music_sel);
    music_play_track(idx);
}

/* 下一首 */
static void music_play_next(void)
{
    if (s_music_count <= 0) return;
    int idx = music_next_index(s_playing_idx >= 0 ? s_playing_idx : s_music_sel);
    music_play_track(idx);
}

/* ========== UI定时器回调（200ms刷新进度条+均衡器+歌词） ========== */
static void music_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_music_obj) return;

    if (s_music_page == MUSIC_PAGE_PLAY) {
        /* 播放页刷新 */
        if (s_is_playing && s_playing_idx >= 0 && s_progress_bar) {
            uint32_t pos_ms = wav_decoder_get_position_ms(&s_wav_decoder);
            const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
            uint32_t dur_ms = fmt ? fmt->duration_ms : 0;

            /* 进度调节模式 / seek 未完成时，用游标显示，避免 200ms 刷新
             * 把用户刚拖到（或刚按键调到）的位置冲掉；歌词仍跟真实进度。 */
            uint32_t disp_ms = s_seek_mode ? s_seek_cursor_ms : pos_ms;
            if (s_seek_req_ms >= 0) disp_ms = (uint32_t)s_seek_req_ms;

            if (dur_ms > 0) {
                int pct = (int)(disp_ms * 100 / dur_ms);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
            }

            /* 更新时间标签 */
            if (s_time_lbl) {
                char buf[24];
                uint32_t pos_s = disp_ms / 1000;
                uint32_t dur_s = dur_ms / 1000;
                snprintf(buf, sizeof(buf), "%02u:%02u/%02u:%02u",
                         (unsigned int)(pos_s / 60), (unsigned int)(pos_s % 60),
                         (unsigned int)(dur_s / 60), (unsigned int)(dur_s % 60));
                lv_label_set_text(s_time_lbl, buf);
            }

            /* 更新歌词（当前行高亮，前后行显示） */
            if (s_lrc_enabled && lrc_parser_is_loaded(&s_lrc)) {
                int cur_line = lrc_parser_get_line_at(&s_lrc, pos_ms);
                for (int i = 0; i < MUSIC_LRC_LINES; i++) {
                    if (!s_lrc_labels[i]) continue;
                    int line_idx = cur_line - (MUSIC_LRC_LINES / 2) + i;
                    const lrc_line_t *line = lrc_parser_get_line(&s_lrc, line_idx);
                    const theme_colors_t *colors = ui_theme_colors();
                    if (line) {
                        lv_label_set_text(s_lrc_labels[i], line->text);
                        if (i == MUSIC_LRC_LINES / 2) {
                            /* 当前行高亮 */
                            lv_obj_set_style_text_color(s_lrc_labels[i], lv_color_hex(colors->text), 0);
                        } else {
                            lv_obj_set_style_text_color(s_lrc_labels[i], lv_color_hex(colors->text_dim), 0);
                        }
                    } else {
                        lv_label_set_text(s_lrc_labels[i], "");
                    }
                }
            }

            /* 频谱动画（伪随机跳动模拟频谱） */
            if (s_eq_enabled) {
                for (int i = 0; i < 4; i++) {
                    if (s_eq_bars[i]) {
                        int val = 20 + (rand() % 60);
                        lv_bar_set_value(s_eq_bars[i], val, LV_ANIM_OFF);
                        lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }
        }
    } else if (s_music_page == MUSIC_PAGE_LIST) {
        /* 列表页刷新进度条（seek 请求处理中暂停刷新，避免来回跳） */
        if (s_progress_bar && s_is_playing && s_playing_idx >= 0 && s_seek_req_ms < 0) {
            uint32_t pos_ms = wav_decoder_get_position_ms(&s_wav_decoder);
            const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
            uint32_t dur_ms = fmt ? fmt->duration_ms : 0;
            if (dur_ms > 0) {
                int pct = (int)(pos_ms * 100 / dur_ms);
                if (pct > 100) pct = 100;
                lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
            }
        }
    }
}

/* ========== 频谱辅助函数（简易FFT模拟） ========== */

/* 更新频谱条（根据播放状态） */
static void music_update_eq_visual(void)
{
    if (!s_eq_enabled) {
        for (int i = 0; i < 4; i++) {
            if (s_eq_bars[i]) {
                lv_bar_set_value(s_eq_bars[i], 0, LV_ANIM_OFF);
                lv_obj_add_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (s_eq_bars[i]) {
            int val = s_is_playing && !s_is_paused ? 20 + (rand() % 60) : 0;
            lv_bar_set_value(s_eq_bars[i], val, LV_ANIM_OFF);
            if (s_is_playing && !s_is_paused) {
                lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ========== UI刷新 ========== */

/* 显示文件名：从完整路径取 basename 并去掉扩展名 */
static void music_get_display_name(const char *name, char *out, size_t out_size)
{
    if (out_size == 0) return;
    /* 取最后一个 '/' 之后的文件名 */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* 进度条触摸/拖动回调：把点击的横坐标换算成百分比，提交 seek 请求。
 * 真正的 wav_decoder_seek 在播放任务里执行，这里只写 volatile 请求，线程安全。 */
static void music_progress_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;

    lv_obj_t *bar = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_active();
    if (!bar || !indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t area;
    lv_obj_get_coords(bar, &area);
    int w = (int)(area.x2 - area.x1) + 1;
    if (w <= 0) return;

    int pct = (int)(((int64_t)(p.x - area.x1)) * 100 / w);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);

    if (!s_is_playing || s_playing_idx < 0) return;
    const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
    uint32_t dur_ms = fmt ? fmt->duration_ms : 0;
    if (dur_ms == 0) return;
    s_seek_req_ms = (int32_t)(((uint64_t)pct * dur_ms) / 100);
    ESP_LOGI(TAG, "Touch seek: %d%% -> %d ms", pct, (int)s_seek_req_ms);
}

/* 创建进度条 */
static lv_obj_t* music_create_progress_bar(lv_obj_t *parent, int y)
{
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_t *bar = lv_bar_create(parent);
    if (!bar) {
        ESP_LOGE(TAG, "lv_bar_create(progress) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return NULL;
    }
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_size(bar, LCD_H_RES - 8, MUSIC_PROGRESS_H);
    lv_obj_set_pos(bar, 4, y);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    /* 触摸拖动进度：PRESSED/RELEASED 都换算位置，拖动结束以松手位置为准 */
    lv_obj_add_event_cb(bar, music_progress_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bar, music_progress_event_cb, LV_EVENT_RELEASED, NULL);
    return bar;
}
/* 创建频谱条 */
static void music_create_eq_bars(lv_obj_t *parent, int y)
{
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_t *eq_container = lv_obj_create(parent);
    if (!eq_container) {
        ESP_LOGE(TAG, "lv_obj_create(eq_container) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(eq_container);
    lv_obj_set_pos(eq_container, 0, y);
    lv_obj_set_size(eq_container, LCD_H_RES, MUSIC_EQ_H);
    lv_obj_clear_flag(eq_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(eq_container, LV_OPA_TRANSP, 0);

    lv_obj_t *eq_label = lv_label_create(eq_container);
    if (eq_label) {
        lv_label_set_text(eq_label, "BASS");
        lv_obj_set_style_text_color(eq_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(eq_label, lv_font_cn_get(14), 0);
        lv_obj_align(eq_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    }
    lv_obj_t *treble_label = lv_label_create(eq_container);
    if (treble_label) {
        lv_label_set_text(treble_label, "TREBLE");
        lv_obj_set_style_text_color(treble_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(treble_label, lv_font_cn_get(14), 0);
        lv_obj_align(treble_label, LV_ALIGN_BOTTOM_RIGHT, -4, 0);
    }
    int eq_bar_w = (LCD_H_RES - 24) / 4;
    for (int i = 0; i < 4; i++) {
        s_eq_bars[i] = lv_bar_create(eq_container);
        if (!s_eq_bars[i]) {
            ESP_LOGE(TAG, "lv_bar_create(eq_bar) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(s_eq_bars[i]);
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(colors->border), 0);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_eq_bars[i], 1, 0);
        uint32_t eq_color = (i == 0 || i == 1) ? colors->text : colors->sel_bg;
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(eq_color), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_eq_bars[i], 1, LV_PART_INDICATOR);
        lv_obj_set_size(s_eq_bars[i], eq_bar_w, MUSIC_EQ_H - 2);
        lv_obj_set_pos(s_eq_bars[i], 8 + i * (eq_bar_w + 2), 0);
        lv_bar_set_range(s_eq_bars[i], 0, 100);
        lv_bar_set_value(s_eq_bars[i], 0, LV_ANIM_OFF);
        lv_obj_add_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ========== 列表页刷新 ========== */
static void music_refresh_list_page(void)
{
    if (!s_music_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_music_obj);
    s_progress_bar = NULL;
    for (int i = 0; i < 4; i++) s_eq_bars[i] = NULL;
    s_status_lbl = NULL;
    s_vol_lbl = NULL;
    s_time_lbl = NULL;
    for (int i = 0; i < MUSIC_LRC_LINES; i++) s_lrc_labels[i] = NULL;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_music_row_h = font_px + 1;

    /* ========== 区1：顶部状态区 ========== */
    lv_obj_t *header = lv_obj_create(s_music_obj);
    if (!header) {
        ESP_LOGE(TAG, "lv_obj_create(header) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lv_obj_remove_style_all(header);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_set_size(header, LCD_H_RES, MUSIC_HEADER_H);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    }
    char status[64];
    if (s_is_playing && s_playing_idx >= 0) {
        char display_name[MUSIC_DISPLAY_LEN];
        music_get_display_name(s_music_entries[s_playing_idx], display_name, sizeof(display_name));
        /* 切到其它 App/桌面后仍在播放，列表页状态栏标注"后台播放" */
        snprintf(status, sizeof(status), "%s %s · %s",
                 s_is_paused ? "⏸" : "▶", display_name,
                 lang_get(STR_MUSIC_BACKGROUND));
    } else {
        snprintf(status, sizeof(status), "%s", lang_get(STR_MUSIC_LIST));
    }
    s_status_lbl = lv_label_create(header);
    if (s_status_lbl) {
        lv_label_set_text(s_status_lbl, status);
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(s_status_lbl, lv_font_cn_get(14), 0);
        lv_obj_set_width(s_status_lbl, LCD_H_RES - 60);
        lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(s_status_lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", s_volume);
    s_vol_lbl = lv_label_create(header);
    if (s_vol_lbl) {
        lv_label_set_text(s_vol_lbl, vol_str);
        lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(s_vol_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(s_vol_lbl, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    /* ========== 区2：进度条 ========== */
    s_progress_bar = music_create_progress_bar(s_music_obj, MUSIC_HEADER_H + 2);
    /* ========== 区3：频谱 ========== */
    music_create_eq_bars(s_music_obj, MUSIC_HEADER_H + MUSIC_PROGRESS_H + 4);
    /* ========== 区4：分隔线 ========== */
    int divider_y = MUSIC_HEADER_H + MUSIC_PROGRESS_H + MUSIC_EQ_H + 6;
    lv_obj_t *divider = lv_obj_create(s_music_obj);
    if (divider) {
        lv_obj_remove_style_all(divider);
        lv_obj_set_pos(divider, 4, divider_y);
        lv_obj_set_size(divider, LCD_H_RES - 8, MUSIC_DIVIDER_H);
        lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(divider, lv_color_hex(colors->border), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);
    }

    /* ========== 区5：文件列表 ========== */
    int list_y = MUSIC_LIST_TOP;
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H - list_y;
    s_music_vis_rows = avail_h / s_music_row_h;
    if (s_music_vis_rows > 5) s_music_vis_rows = 5;
    if (s_music_vis_rows < 1) s_music_vis_rows = 1;

    if (s_music_sel < s_music_scroll) s_music_scroll = s_music_sel;
    if (s_music_sel >= s_music_scroll + s_music_vis_rows)
        s_music_scroll = s_music_sel - s_music_vis_rows + 1;
    if (s_music_scroll < 0) s_music_scroll = 0;
    if (s_music_scroll > s_music_count - s_music_vis_rows)
        s_music_scroll = s_music_count - s_music_vis_rows;
    if (s_music_scroll < 0) s_music_scroll = 0;

    if (s_music_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_music_obj);
        if (empty_lbl) {
            /* 未插 SD 卡时给出明确提示，避免与“卡内无音乐”混淆 */
            const char *empty_tip = app_sys_sdcard_ready()
                                        ? lang_get(STR_MUSIC_NO_FILE)
                                        : lang_get(STR_MP_NO_SD_CARD);
            lv_label_set_text(empty_lbl, empty_tip);
            lv_obj_set_style_text_color(empty_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_style_text_font(empty_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 10);
        }
        return;
    }
    for (int i = 0; i < s_music_vis_rows; i++) {
        int idx = s_music_scroll + i;
        if (idx >= s_music_count) break;
        int row_y = list_y + i * s_music_row_h;
        lv_obj_t *row = lv_obj_create(s_music_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(music_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_y);
        lv_obj_set_size(row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_music_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        char display_name[MUSIC_DISPLAY_LEN];
        music_get_display_name(s_music_entries[idx], display_name, sizeof(display_name));
        const char *playing_mark = (idx == s_playing_idx && s_is_playing) ? "▶" : " ";
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %.28s", playing_mark, display_name);
        lv_obj_t *lbl = lv_label_create(row);
        if (lbl) {
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_width(lbl, LCD_H_RES - 8);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        }
    }
    /* ========== 区6：底部操作提示 ========== */
    int hint_y = list_y + s_music_vis_rows * s_music_row_h;
    if (hint_y + s_music_row_h < LCD_V_RES - DOCK_H) {
        lv_obj_t *hint_row = lv_obj_create(s_music_obj);
        if (hint_row) {
            lv_obj_remove_style_all(hint_row);
            lv_obj_set_pos(hint_row, 0, hint_y);
            lv_obj_set_size(hint_row, LCD_H_RES, s_music_row_h);
            lv_obj_clear_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(hint_row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *hint_lbl = hint_row ? lv_label_create(hint_row) : NULL;
        if (hint_lbl) {
            lv_label_set_text(hint_lbl, lang_get(STR_MUSIC_HINT));
            lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
            lv_obj_align(hint_lbl, LV_ALIGN_CENTER, 0, 0);
        }
    }

    music_update_eq_visual();
}

/* ========== 播放页刷新 ========== */
static void music_refresh_play_page(void)
{
    if (!s_music_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_music_obj);
    s_progress_bar = NULL;
    for (int i = 0; i < 4; i++) s_eq_bars[i] = NULL;
    s_status_lbl = NULL;
    s_vol_lbl = NULL;
    s_time_lbl = NULL;
    for (int i = 0; i < MUSIC_LRC_LINES; i++) s_lrc_labels[i] = NULL;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    int y = 0;

    /* ========== 歌曲信息区 ========== */
    char title[64] = "";
    int cur_idx = (s_playing_idx >= 0) ? s_playing_idx : s_music_sel;
    if (s_music_count > 0 && cur_idx >= 0 && cur_idx < s_music_count) {
        music_get_display_name(s_music_entries[cur_idx], title, sizeof(title));
    } else {
        snprintf(title, sizeof(title), "%s", lang_get(STR_MUSIC_PLAYING));
    }

    /* 标题（当前歌曲名） */
    lv_obj_t *title_lbl = lv_label_create(s_music_obj);
    if (title_lbl) {
        lv_label_set_text(title_lbl, title);
        lv_obj_set_style_text_color(title_lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(title_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(title_lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 2);
    }
    y += font_px + 4;
    /* 歌词区（4行） */
    if (s_lrc_enabled && lrc_parser_is_loaded(&s_lrc)) {
        lv_obj_t *lrc_area = lv_obj_create(s_music_obj);
        if (lrc_area) {
            lv_obj_remove_style_all(lrc_area);
            lv_obj_set_pos(lrc_area, 0, y);
            lv_obj_set_size(lrc_area, LCD_H_RES, MUSIC_LRC_LINES * (font_px + 1));
            lv_obj_clear_flag(lrc_area, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(lrc_area, LV_OPA_TRANSP, 0);
            for (int i = 0; i < MUSIC_LRC_LINES; i++) {
                lv_obj_t *lbl = lv_label_create(lrc_area);
                if (!lbl) continue;
                lv_obj_set_style_text_font(lbl, lv_font_cn_get(14), 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
                lv_obj_set_width(lbl, LCD_H_RES - 12);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_label_set_text(lbl, "");
                lv_obj_set_pos(lbl, 6, i * (font_px + 1));
                s_lrc_labels[i] = lbl;
            }
        }
        y += MUSIC_LRC_LINES * (font_px + 1) + 2;
    } else {
        /* 无歌词时显示提示 */
        lv_obj_t *no_lrc = lv_label_create(s_music_obj);
        if (no_lrc) {
            /* 歌词功能开着但没找到同名 .lrc -> "暂无歌词"；
             * 仅当用户主动关歌词时才显示"歌词:关" */
            lv_label_set_text(no_lrc,
                s_lrc_enabled ? lang_get(STR_MUSIC_NO_LRC) : lang_get(STR_MUSIC_LRC_OFF));
            lv_obj_set_style_text_color(no_lrc, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_style_text_font(no_lrc, lv_font_cn_get(14), 0);
            lv_obj_align(no_lrc, LV_ALIGN_TOP_MID, 0, y + 4);
        }
        y += font_px + 16;
    }
    /* 进度条 */
    s_progress_bar = music_create_progress_bar(s_music_obj, y);
    y += MUSIC_PROGRESS_H + 2;
    /* 时间标签 */
    char time_str[24];
    uint32_t pos_ms = wav_decoder_get_position_ms(&s_wav_decoder);
    const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
    uint32_t dur_ms = fmt ? fmt->duration_ms : 0;
    uint32_t pos_s = pos_ms / 1000;
    uint32_t dur_s = dur_ms / 1000;
    snprintf(time_str, sizeof(time_str), "%02u:%02u/%02u:%02u",
             (unsigned int)(pos_s / 60), (unsigned int)(pos_s % 60),
             (unsigned int)(dur_s / 60), (unsigned int)(dur_s % 60));
    s_time_lbl = lv_label_create(s_music_obj);
    if (s_time_lbl) {
        lv_label_set_text(s_time_lbl, time_str);
        lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(s_time_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -4, y - MUSIC_PROGRESS_H / 2);
    }
    y += font_px - 2;
    /* 循环模式 + 播放状态 */
    char mode_str[32];
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_SINGLE)); break;
    case PLAY_MODE_LIST_LOOP: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_LIST)); break;
    case PLAY_MODE_RANDOM: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_RANDOM)); break;
    case PLAY_MODE_ORDER: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_ORDER)); break;
    default: mode_str[0] = '\0'; break;
    }
    lv_obj_t *mode_lbl = lv_label_create(s_music_obj);
    if (mode_lbl) {
        lv_label_set_text(mode_lbl, mode_str);
        lv_obj_set_style_text_color(mode_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(mode_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(mode_lbl, LV_ALIGN_LEFT_MID, 4, y);
    }
    /* 播放/暂停状态 */
    lv_obj_t *play_lbl = lv_label_create(s_music_obj);
    if (play_lbl) {
        lv_label_set_text(play_lbl, s_is_paused ? lang_get(STR_MUSIC_PAUSE) : lang_get(STR_MUSIC_PLAY));
        lv_obj_set_style_text_color(play_lbl, lv_color_hex(colors->sel_bg), 0);
        lv_obj_set_style_text_font(play_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(play_lbl, LV_ALIGN_RIGHT_MID, -4, y);
    }
    y += font_px + 4;
    /* 频谱（播放页） */
    if (s_eq_enabled) {
        music_create_eq_bars(s_music_obj, y);
        y += MUSIC_EQ_H + 2;
    }
    /* 底部操作提示 */
    lv_obj_t *hint_lbl = lv_label_create(s_music_obj);
    if (hint_lbl) {
        lv_label_set_text(hint_lbl,
            s_seek_mode ? lang_get(STR_MUSIC_SEEK) : lang_get(STR_MUSIC_PLAY_HINT2));
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H);
    }
    music_update_eq_visual();
}

/* EQ 预设显示名（直接使用已加好的 lang 字符串） */
static const char *music_eq_preset_name(music_eq_preset_t p)
{
    switch (p) {
    case MUSIC_EQ_POP:       return lang_get(STR_MUSIC_EQ_POP);
    case MUSIC_EQ_ROCK:      return lang_get(STR_MUSIC_EQ_ROCK);
    case MUSIC_EQ_CLASSICAL: return lang_get(STR_MUSIC_EQ_CLASSICAL);
    case MUSIC_EQ_BASS:      return lang_get(STR_MUSIC_EQ_BASS);
    case MUSIC_EQ_OFF:
    default:                 return lang_get(STR_MUSIC_EQ_OFF);
    }
}

/* ========== 设置页刷新 ========== */
static void music_refresh_settings_page(void)
{
    if (!s_music_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_music_obj);
    s_progress_bar = NULL;
    for (int i = 0; i < 4; i++) s_eq_bars[i] = NULL;
    s_status_lbl = NULL;
    s_vol_lbl = NULL;
    s_time_lbl = NULL;
    for (int i = 0; i < MUSIC_LRC_LINES; i++) s_lrc_labels[i] = NULL;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 4;

    /* 标题 */
    lv_obj_t *title_lbl = lv_label_create(s_music_obj);
    if (title_lbl) {
        lv_label_set_text(title_lbl, lang_get(STR_MUSIC_SETTINGS));
        lv_obj_set_style_text_color(title_lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(title_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 4);
    }
    int y = font_px + 10;

    /* 设置项行 */
    const char *labels[SETTINGS_ITEM_COUNT];
    char values[SETTINGS_ITEM_COUNT][32];

    labels[0] = lang_get(STR_VOLUME);
    snprintf(values[0], sizeof(values[0]), "%d%%", s_volume);

    labels[1] = lang_get(STR_MUSIC_SPECTRUM);
    snprintf(values[1], sizeof(values[1]), "%s", s_eq_enabled ? lang_get(STR_MUSIC_SPECTRUM_ON) : lang_get(STR_MUSIC_SPECTRUM_OFF));

    labels[2] = lang_get(STR_MUSIC_LOOP_MODE);
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE: snprintf(values[2], sizeof(values[2]), "%s", lang_get(STR_MUSIC_LOOP_SINGLE)); break;
    case PLAY_MODE_LIST_LOOP: snprintf(values[2], sizeof(values[2]), "%s", lang_get(STR_MUSIC_LOOP_LIST)); break;
    case PLAY_MODE_RANDOM: snprintf(values[2], sizeof(values[2]), "%s", lang_get(STR_MUSIC_LOOP_RANDOM)); break;
    case PLAY_MODE_ORDER: snprintf(values[2], sizeof(values[2]), "%s", lang_get(STR_MUSIC_ORDER)); break;
    default: values[2][0] = '\0'; break;
    }

    labels[3] = lang_get(STR_AUDIO_OUTPUT);
    {
        audio_device_info_t devs[AUDIO_OUT_MAX];
        int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
        audio_out_type_t active = audio_output_get_active();
        const char *dev_name = "None";
        for (int i = 0; i < dev_count; i++) {
            if (devs[i].type == active) {
                dev_name = devs[i].name;
                break;
            }
        }
        snprintf(values[3], sizeof(values[3]), "%s", dev_name);
    }

    labels[4] = lang_get(STR_MUSIC_EQ);
    snprintf(values[4], sizeof(values[4]), "%s", music_eq_preset_name(s_eq_preset));

    /* 5: 播放队列——显示当前正在播放的曲目名（无播放显示 -） */
    labels[5] = lang_get(STR_MUSIC_QUEUE);
    if (s_playing_idx >= 0 && s_playing_idx < s_music_count) {
        const char *full = s_music_entries[s_playing_idx];
        const char *base = strrchr(full, '/');
        base = base ? base + 1 : full;
        snprintf(values[5], sizeof(values[5]), "%s", base);
    } else {
        snprintf(values[5], sizeof(values[5]), "-");
    }

    /* 6: 清空队列（A 键执行） */
    labels[6] = lang_get(STR_MUSIC_QUEUE_CLEAR);
    values[6][0] = '\0';

    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_music_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(settings_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            y += row_h;
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_settings_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (lbl) {
            lv_label_set_text(lbl, labels[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
        }
        lv_obj_t *val_lbl = lv_label_create(row);
        if (val_lbl) {
            lv_label_set_text(val_lbl, values[i]);
            lv_obj_set_style_text_color(val_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_style_text_font(val_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
        }
        y += row_h;
    }
    /* 底部提示 */
    lv_obj_t *hint_lbl = lv_label_create(s_music_obj);
    if (hint_lbl) {
        lv_label_set_text(hint_lbl, lang_get(STR_MUSIC_SETTINGS_HINT));
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H);
    }
}

/* 根据当前页面刷新 */
static void music_refresh_page(void)
{
    if (!s_music_obj) return;
    switch (s_music_page) {
    case MUSIC_PAGE_LIST:     music_refresh_list_page(); break;
    case MUSIC_PAGE_PLAY:     music_refresh_play_page(); break;
    case MUSIC_PAGE_SETTINGS: music_refresh_settings_page(); break;
    default: break;
    }
}

/* ========== 页面生命周期 ========== */
static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music app init (3-page player)");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_MUSIC));

    /* 从 NVS 加载音乐设置（统一走配置门面层，保持原有默认值语义） */
    s_volume = audio_output_get_volume();
    /* EQ 预设走 APP_CFG_MUSIC_EQ；频谱开关走本文件专属 NVS 键 */
    music_load_eq_cfg();
    s_lrc_enabled = app_sys_cfg_get_int(APP_CFG_MUSIC_LRC, 1) != 0;
    int saved_mode = app_sys_cfg_get_int(APP_CFG_MUSIC_MODE, PLAY_MODE_SINGLE);
    if (saved_mode >= 0 && saved_mode < PLAY_MODE_MAX) {
        s_play_mode = (play_mode_t)saved_mode;
    }

    lv_obj_t *list = lv_obj_create(scr);
    if (!list) {
        ESP_LOGE(TAG, "lv_obj_create(music_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_music_obj = list;
    s_music_sel = 0;
    s_music_scroll = 0;
    s_music_page = MUSIC_PAGE_LIST;
    music_scan_auto();
    music_refresh_page();
    if (s_ui_timer) lv_timer_del(s_ui_timer);
    s_ui_timer = lv_timer_create(music_ui_timer_cb, 200, NULL);

    ui_dock_create(scr, 1, 0);
}

static void music_destroy(void)
{
    ESP_LOGI(TAG, "Music destroy (background playback kept: %d)", (int)s_is_playing);
    /* 关键：ui_stack_pop 会调用 destroy，切到桌面/其它 App 时【不停止】播放。
     * 播放状态(s_is_playing/s_playing_idx/s_wav_decoder)与后台任务均为全局持久，
     * 这里只销毁 UI 对象与定时器；真正停止只发生在：
     *   1) 播放任务正常结束（含顺序播放到末尾）；2) 用户/诊断主动调用 music_stop_playback。 */
    if (s_ui_timer) {
        lv_timer_del(s_ui_timer);
        s_ui_timer = NULL;
    }
    s_music_obj = NULL;
    s_progress_bar = NULL;
    for (int i = 0; i < 4; i++) s_eq_bars[i] = NULL;
    s_status_lbl = NULL;
    s_vol_lbl = NULL;
    s_time_lbl = NULL;
    for (int i = 0; i < MUSIC_LRC_LINES; i++) s_lrc_labels[i] = NULL;
    /* 不清 s_music_count：后台播放切歌时 music_next_index() 仍需要列表 */
    if (!s_is_playing) {
        lrc_parser_clear(&s_lrc);
    }
    s_seek_mode = false;   /* 离开页面时退出进度调节模式 */
}

/* ========== 按键处理 ========== */
static bool music_on_key(int key)
{
    switch (s_music_page) {
    /* ===== 列表页 ===== */
    case MUSIC_PAGE_LIST:
        if (key == KEY_B) {
            if (ui_stack_depth() > 1) ui_stack_pop();
            return true;
        }
        if (s_music_count == 0) {
            /* 空列表：RIGHT进入设置页 */
            if (key == KEY_RIGHT) {
                s_music_page = MUSIC_PAGE_SETTINGS;
                music_refresh_page();
            }
            return true;
        }
        if (key == KEY_UP) {
            s_music_sel = (s_music_sel - 1 + s_music_count) % s_music_count;
            music_refresh_page();
            return true;
        }
        if (key == KEY_DOWN) {
            s_music_sel = (s_music_sel + 1) % s_music_count;
            music_refresh_page();
            return true;
        }
        if (key == KEY_A) {
            /* 播放选中曲目并进入播放页 */
            music_play_track(s_music_sel);
            s_music_page = MUSIC_PAGE_PLAY;
            music_refresh_page();
            return true;
        }
        if (key == KEY_RIGHT) {
            /* 进入播放页 */
            s_music_page = MUSIC_PAGE_PLAY;
            music_refresh_page();
            return true;
        }
        if (key == KEY_LEFT) {
            /* 进入设置页 */
            s_music_page = MUSIC_PAGE_SETTINGS;
            music_refresh_page();
            return true;
        }
        return true;

    /* ===== 播放页 ===== */
    case MUSIC_PAGE_PLAY:
        if (s_seek_mode) {
            /* ---- 进度调节模式（5键无触摸设备的必须路径） ----
             * LEFT/RIGHT：∓5 秒   UP/DOWN：∓30 秒
             * A：确认跳转并退出   B：取消并退出 */
            if (key == KEY_A) {
                s_seek_req_ms = (int32_t)s_seek_cursor_ms;
                s_seek_mode = false;
                ESP_LOGI(TAG, "Seek confirmed: %u ms", (unsigned)s_seek_cursor_ms);
                music_refresh_page();
                return true;
            }
            if (key == KEY_B) {
                s_seek_mode = false;   /* 取消，不提交 seek */
                music_refresh_page();
                return true;
            }
            if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN) {
                const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
                uint32_t dur_ms = fmt ? fmt->duration_ms : 0;
                int delta = (key == KEY_LEFT || key == KEY_RIGHT) ? 5000 : 30000;
                if (key == KEY_LEFT || key == KEY_UP) {
                    s_seek_cursor_ms = (s_seek_cursor_ms > (uint32_t)delta)
                                         ? s_seek_cursor_ms - (uint32_t)delta : 0;
                } else {
                    s_seek_cursor_ms += (uint32_t)delta;
                    if (dur_ms > 0 && s_seek_cursor_ms > dur_ms) s_seek_cursor_ms = dur_ms;
                }
                music_refresh_page();   /* 刷新提示行；进度条由 UI 定时器按游标刷新 */
                return true;
            }
            return true;   /* 调节模式屏蔽其它键，避免误切歌 */
        }
        if (key == KEY_B) {
            /* 返回列表页（不停止播放，后台继续） */
            s_music_page = MUSIC_PAGE_LIST;
            music_refresh_page();
            return true;
        }
        if (key == KEY_A) {
            /* A：未播放时开始播放；播放中进入进度调节模式 */
            if (!s_is_playing) {
                if (s_music_count > 0) {
                    int idx = (s_playing_idx >= 0) ? s_playing_idx : s_music_sel;
                    music_play_track(idx);
                    music_refresh_page();
                }
                return true;
            }
            s_seek_cursor_ms = wav_decoder_get_position_ms(&s_wav_decoder);
            {
                const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
                uint32_t dur_ms = fmt ? fmt->duration_ms : 0;
                if (dur_ms > 0 && s_seek_cursor_ms > dur_ms) s_seek_cursor_ms = dur_ms;
                if (s_seek_req_ms >= 0) s_seek_cursor_ms = (uint32_t)s_seek_req_ms;
            }
            s_seek_mode = true;
            music_refresh_page();
            return true;
        }
        if (key == KEY_LEFT) {
            /* 上一首（保留原操作） */
            music_play_prev();
            music_refresh_page();
            return true;
        }
        if (key == KEY_RIGHT) {
            /* 下一首（保留原操作） */
            music_play_next();
            music_refresh_page();
            return true;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            /* 播放/暂停：原播放页的循环模式切换移到设置页，腾出按键给进度调节 */
            if (s_is_playing) {
                s_is_paused = !s_is_paused;
                if (s_is_paused) audio_output_stop();
                if (s_bg_task_id >= 0) {
                    bg_tasks_set_state(s_bg_task_id,
                        s_is_paused ? BG_TASK_STATE_PAUSED : BG_TASK_STATE_RUNNING);
                }
                music_refresh_page();
            }
            return true;
        }
        return true;

    /* ===== 设置页 ===== */
    case MUSIC_PAGE_SETTINGS:
    default:
        if (key == KEY_B) {
            /* 返回列表页 */
            s_music_page = MUSIC_PAGE_LIST;
            music_refresh_page();
            return true;
        }
        if (key == KEY_UP) {
            if (s_settings_sel > 0) {
                s_settings_sel--;
                music_refresh_page();
            }
            return true;
        }
        if (key == KEY_DOWN) {
            if (s_settings_sel < SETTINGS_ITEM_COUNT - 1) {
                s_settings_sel++;
                music_refresh_page();
            }
            return true;
        }
        if (key == KEY_A) {
            switch (s_settings_sel) {
            case 0: /* 音量 */
                break;
            case 1: /* 频谱动画开关（独立 NVS 键，不占 EQ 预设值） */
                s_eq_enabled = !s_eq_enabled;
                music_save_spectrum();
                music_refresh_page();
                break;
            case 2: /* 循环模式 */
                s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
                app_sys_cfg_set_int(APP_CFG_MUSIC_MODE, (int)s_play_mode);
                music_refresh_page();
                break;
            case 3: /* 播放输出 */
                {
                    /* 循环切换输出设备 */
                    audio_device_info_t devs[AUDIO_OUT_MAX];
                    int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
                    audio_out_type_t active = audio_output_get_active();
                    int active_idx = 0;
                    for (int i = 0; i < dev_count; i++) {
                        if (devs[i].type == active) {
                            active_idx = i;
                            break;
                        }
                    }
                    /* 找下一个可用设备 */
                    for (int step = 1; step <= dev_count; step++) {
                        int next = (active_idx + step) % dev_count;
                        if (devs[next].available) {
                            audio_output_set_active(devs[next].type);
                            sys_nvs_save_audio_output(devs[next].type);
                            sys_nvs_save_audio_auto(false);
                            break;
                        }
                    }
                    music_refresh_page();
                }
                break;
            case 4: /* 音效预设：OFF->POP->ROCK->CLASSICAL->BASS->OFF */
                music_eq_set_preset((music_eq_preset_t)((s_eq_preset + 1) % MUSIC_EQ_MAX));
                music_refresh_page();
                break;
            case 5: /* 播放队列：回到列表页并定位当前播放曲 */
                if (s_playing_idx >= 0 && s_playing_idx < s_music_count) {
                    s_music_sel = s_playing_idx;   /* 滚动由列表页渲染自动适配 */
                    s_music_page = MUSIC_PAGE_LIST;
                    music_refresh_page();
                }
                break;
            case 6: /* 清空队列：停止播放并清空列表（不删文件，重新扫描可恢复） */
                music_stop_playback();
                s_music_count = 0;
                s_music_sel = 0;
                s_music_scroll = 0;
                music_refresh_page();
                break;
            }
            return true;
        }
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            switch (s_settings_sel) {
            case 0: /* 音量调节 */
                if (key == KEY_LEFT) {
                    s_volume = (s_volume >= 10) ? s_volume - 10 : 0;
                } else {
                    s_volume = (s_volume <= 90) ? s_volume + 10 : 100;
                }
                audio_output_set_volume(s_volume);
                sys_nvs_save_volume(s_volume);
                music_refresh_page();
                break;
            case 1: /* 频谱动画开关（独立 NVS 键，不占 EQ 预设值） */
                s_eq_enabled = !s_eq_enabled;
                music_save_spectrum();
                music_refresh_page();
                break;
            case 2: /* 循环模式 */
                s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
                app_sys_cfg_set_int(APP_CFG_MUSIC_MODE, (int)s_play_mode);
                music_refresh_page();
                break;
            case 3: /* 播放输出 */
                {
                    audio_device_info_t devs[AUDIO_OUT_MAX];
                    int dev_count = audio_output_get_devices(devs, AUDIO_OUT_MAX);
                    audio_out_type_t active = audio_output_get_active();
                    int active_idx = 0;
                    for (int i = 0; i < dev_count; i++) {
                        if (devs[i].type == active) {
                            active_idx = i;
                            break;
                        }
                    }
                    for (int step = 1; step <= dev_count; step++) {
                        int next = (active_idx + step) % dev_count;
                        if (devs[next].available) {
                            audio_output_set_active(devs[next].type);
                            sys_nvs_save_audio_output(devs[next].type);
                            sys_nvs_save_audio_auto(false);
                            break;
                        }
                    }
                    music_refresh_page();
                }
                break;
            case 4: /* 音效预设：左右键双向循环切换 */
                {
                    int p = (int)s_eq_preset + ((key == KEY_RIGHT) ? 1 : (MUSIC_EQ_MAX - 1));
                    music_eq_set_preset((music_eq_preset_t)(p % MUSIC_EQ_MAX));
                    music_refresh_page();
                }
                break;
            }
            return true;
        }
        return true;
    }
}

/* ========== 诊断控制接口（XTC cmd=15/16 使用） ==========
 * UART 命令运行在 PSRAM 栈，不能直接做 FAT 读/LRC 加载；
 * 这里用一个内部 RAM 栈的轻量任务异步执行扫描/播放控制，且与 UI 解耦。 */
#define MUSIC_CTRL_TASK_STACK 8192
static StackType_t s_music_ctrl_stack[MUSIC_CTRL_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_music_ctrl_tcb;
static TaskHandle_t s_music_ctrl_task = NULL;
static volatile int s_music_ctrl_op = 0;
static volatile int s_music_ctrl_idx = 0;

static void music_ctrl_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int op = s_music_ctrl_op;
        int idx = s_music_ctrl_idx;
        s_music_ctrl_op = 0;

        if (op == 1) {
            music_scan_auto();
        } else if (op == 2) {
            if (s_music_count == 0) music_scan_auto();
            music_start_playback(idx);
        } else if (op == 3) {
            music_stop_playback();
        }
    }
}

void app_music_debug_control(int op, int idx)
{
    if (!s_music_ctrl_task) {
        s_music_ctrl_task = xTaskCreateStatic(
            music_ctrl_task, "music_ctrl",
            MUSIC_CTRL_TASK_STACK / sizeof(StackType_t),
            NULL, 12, s_music_ctrl_stack, &s_music_ctrl_tcb);
        if (!s_music_ctrl_task) return;
    }
    s_music_ctrl_idx = idx;
    s_music_ctrl_op = op;       /* volatile 最后写入，作为提交标志 */
    xTaskNotifyGive(s_music_ctrl_task);
}

void app_music_get_status(app_music_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    strncpy(out->dir, s_music_current_path, sizeof(out->dir) - 1);
    out->count = s_music_count;
    out->selected = s_music_sel;
    out->playing_idx = (int)s_playing_idx;
    out->is_playing = s_is_playing;
    out->is_paused = s_is_paused;
    out->src = (int)s_wav_decoder.src;
    out->sample_rate = (int)s_wav_decoder.format.sample_rate;
    out->channels = (int)s_wav_decoder.format.channels;
    out->bytes_read = s_wav_decoder.bytes_read;
    out->position_ms = wav_decoder_get_position_ms(&s_wav_decoder);
    out->probe_count = (int)s_wav_decoder.probe_count;
    for (int i = 0; i < 16; i++) out->probe[i] = s_wav_decoder.probe[i];
    out->probe_peak = s_wav_decoder.probe_peak;
    out->probe_offset = (int)s_wav_decoder.probe_offset;
    out->probe_first_peak = s_wav_decoder.probe_first_peak;
    for (int i = 0; i < 8; i++) out->first_samples[i] = s_wav_decoder.first_samples[i];
    audio_decoder_get_mp3_table_probe(out->table_probe);
    audio_decoder_get_mp3_deep_probe(out->deep_probe);
    out->probe_max_peak = s_wav_decoder.probe_max_peak;
    out->probe_last_peak = s_wav_decoder.probe_last_peak;
    out->probe_reads = s_wav_decoder.probe_reads;

    int idx = s_playing_idx >= 0 ? (int)s_playing_idx : s_music_sel;
    if (idx >= 0 && idx < s_music_count) {
        /* 测试脚本按文件名匹配，故返回 basename 而非完整路径 */
        const char *base = strrchr(s_music_entries[idx], '/');
        base = base ? base + 1 : s_music_entries[idx];
        strncpy(out->current_file, base, sizeof(out->current_file) - 1);
    }
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};