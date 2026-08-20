/**
 * @file app_music.c
 * @brief 音乐应用 - 三页面架构（列表/播放/设置）
 *
 * 功能：
 * - 列表页：自动扫描存储卡音频文件（/sdcard/music 目录，支持 .wav）
 * - 播放页：歌词显示(LRC)、频谱可视化、播放进度、音量、循环模式、上下一首、暂停
 * - 设置页：音量、频谱开关、循环模式、播放输出设备
 *
 * 操作说明：
 *   列表页：UP/DOWN选择  A播放  RIGHT进入播放页  LEFT进入设置页  B返回
 *   播放页：A播放/暂停  LEFT上一首  RIGHT下一首  B返回列表
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
#include "ui_framework.h"
#include "driver/drv_audio_output.h"
#include "driver/drv_audio_decoder.h"
#include "driver/drv_lrc.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
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

static const char *TAG = "APP_MUSIC";

#define MUSIC_MAX_ENTRIES 32
#define MUSIC_PATH_LEN    128  /* 文件名长度 */
#define MUSIC_FULLPATH_LEN 256 /* 完整路径长度 */
#define MUSIC_DISPLAY_LEN 32   /* 显示名称长度 */
#define DECODE_BUF_SIZE   512  /* 解码缓冲区大小（16位样本数） */

/* ========== 播放模式 ========== */
typedef enum {
    PLAY_MODE_SINGLE = 0,      /* 单曲循环（播完重播当前） */
    PLAY_MODE_LIST_LOOP,       /* 列表循环 */
    PLAY_MODE_RANDOM,          /* 随机播放 */
    PLAY_MODE_MAX
} play_mode_t;

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
#define SETTINGS_ITEM_COUNT 4  /* 音量/频谱/循环/输出 */

/* 文件列表 */
static char s_music_entries[MUSIC_MAX_ENTRIES][MUSIC_PATH_LEN];
static int s_music_count = 0;
static char s_music_current_path[MUSIC_PATH_LEN] = "/sdcard/music";

/* 播放状态 */
static bool s_is_playing = false;
static bool s_is_paused = false;
static int s_playing_idx = -1;         /* 正在播放的文件索引 */
static int s_volume = 50;
static play_mode_t s_play_mode = PLAY_MODE_SINGLE;
static bool s_eq_enabled = true;       /* 频谱显示开关 */
static bool s_lrc_enabled = true;      /* 歌词显示开关 */

/* 解码器和歌词 */
static wav_decoder_t s_wav_decoder;
static lrc_parser_t s_lrc;
static TaskHandle_t s_play_task = NULL;

/* ========== 文件扫描 ========== */
static void music_scan_dir(const char *path)
{
    s_music_count = 0;
    strncpy(s_music_current_path, path, MUSIC_PATH_LEN - 1);
    s_music_current_path[MUSIC_PATH_LEN - 1] = '\0';

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open music directory: %s", path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_music_count < MUSIC_MAX_ENTRIES) {
        if (entry->d_name[0] == '.') continue;

        /* 支持 .wav 音频文件（后续可扩展 .mp3/.flac） */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".wav") != 0) continue;

        strncpy(s_music_entries[s_music_count], entry->d_name, MUSIC_PATH_LEN - 1);
        s_music_entries[s_music_count][MUSIC_PATH_LEN - 1] = '\0';
        s_music_count++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "Music scan: %d WAV files in %s", s_music_count, path);
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

/* 拼接完整路径（手动拼接避免 -Wformat-truncation：128+1+128=257 > 256） */
static void music_build_path(const char *path, const char *name, char *out, size_t out_size)
{
    size_t plen = strlen(path);
    size_t nlen = strlen(name);
    size_t total = plen + 1 + nlen;
    size_t copy = total;
    if (copy >= out_size) copy = out_size - 1;
    /* 复制路径部分 */
    size_t p_copy = plen;
    if (p_copy > copy) p_copy = copy;
    memcpy(out, path, p_copy);
    size_t pos = p_copy;
    /* 分隔符 */
    if (pos < copy) {
        out[pos] = '/';
        pos++;
    }
    /* 复制文件名 */
    size_t n_copy = nlen;
    if (n_copy > copy - (pos > copy ? copy : pos)) n_copy = copy - pos;
    if (n_copy > 0) {
        memcpy(out + pos, name, n_copy);
        pos += n_copy;
    }
    out[pos] = '\0';
}

/* ========== 播放任务 ========== */
static void music_play_task(void *arg)
{
    int file_idx = (int)(intptr_t)arg;
    char fullpath[MUSIC_FULLPATH_LEN];

    music_build_path(s_music_current_path, s_music_entries[file_idx], fullpath, sizeof(fullpath));

    ESP_LOGI(TAG, "Starting playback: %s", fullpath);

    /* 初始化并打开WAV解码器 */
    wav_decoder_init(&s_wav_decoder);
    if (wav_decoder_open(&s_wav_decoder, fullpath) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", fullpath);
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
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
        vTaskDelete(NULL);
        return;
    }

    /* 解码缓冲区（16位PCM样本） */
    int16_t *decode_buf = (int16_t *)malloc(DECODE_BUF_SIZE * sizeof(int16_t));
    if (!decode_buf) {
        ESP_LOGE(TAG, "Failed to allocate decode buffer");
        audio_output_stop();
        wav_decoder_close(&s_wav_decoder);
        s_is_playing = false;
        s_playing_idx = -1;
        s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_is_playing = true;
    s_is_paused = false;

    while (1) {
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

                music_build_path(s_music_current_path, s_music_entries[file_idx], fullpath, sizeof(fullpath));

                if (wav_decoder_open(&s_wav_decoder, fullpath) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open next file: %s", fullpath);
                    break;
                }

                const wav_format_t *next_fmt = wav_decoder_get_format(&s_wav_decoder);
                audio_output_open(next_fmt->sample_rate, 16, next_fmt->channels);

                /* 加载歌词 */
                char lrc_base[MUSIC_FULLPATH_LEN];
                music_build_path(s_music_current_path, s_music_entries[file_idx], lrc_base, sizeof(lrc_base));
                char *dot = strrchr(lrc_base, '.');
                if (dot) *dot = '\0';
                lrc_parser_load(&s_lrc, lrc_base);
                continue;
            }
            break;
        }

        /* 写入音频输出（通过I2S DAC或蓝牙A2DP） */
        ret = audio_output_write(decode_buf, bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "audio_output_write returned %s", esp_err_to_name(ret));
        }
    }

    /* 清理 */
    free(decode_buf);
    audio_output_stop();
    wav_decoder_close(&s_wav_decoder);

    s_is_playing = false;
    s_playing_idx = -1;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

static void music_start_playback(int idx)
{
    if (idx < 0 || idx >= s_music_count) return;

    /* 停止当前播放 */
    if (s_is_playing) {
        s_is_playing = false;
        s_playing_idx = -1;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_playing_idx = idx;
    s_is_paused = false;

    /* 加载歌词（从文件路径去掉扩展名，找同名的.lrc） */
    lrc_parser_init(&s_lrc);
    {
        char lrc_base[MUSIC_FULLPATH_LEN];
        music_build_path(s_music_current_path, s_music_entries[idx], lrc_base, sizeof(lrc_base));
        char *dot = strrchr(lrc_base, '.');
        if (dot) *dot = '\0';
        lrc_parser_load(&s_lrc, lrc_base);
    }

    xTaskCreate(music_play_task, "music_play", 4096,
                (void*)(intptr_t)idx, 5, &s_play_task);
}

static void music_stop_playback(void)
{
    if (s_is_playing || s_play_task != NULL) {
        s_is_playing = false;
        s_playing_idx = -1;
        s_is_paused = false;
        audio_output_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
        s_play_task = NULL;
    }
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

            if (dur_ms > 0) {
                int pct = (int)(pos_ms * 100 / dur_ms);
                if (pct > 100) pct = 100;
                lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
            }

            /* 更新时间标签 */
            if (s_time_lbl) {
                char buf[24];
                uint32_t pos_s = pos_ms / 1000;
                uint32_t dur_s = dur_ms / 1000;
                snprintf(buf, sizeof(buf), "%02u:%02u/%02u:%02u",
                         pos_s / 60, pos_s % 60, dur_s / 60, dur_s % 60);
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
        /* 列表页刷新进度条 */
        if (s_progress_bar && s_is_playing && s_playing_idx >= 0) {
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

/* 显示文件名（去掉扩展名） */
static void music_get_display_name(const char *name, char *out, size_t out_size)
{
    strncpy(out, name, out_size - 1);
    out[out_size - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

/* 创建进度条 */
static lv_obj_t* music_create_progress_bar(lv_obj_t *parent, int y)
{
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_t *bar = lv_bar_create(parent);
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
    return bar;
}

/* 创建频谱条 */
static void music_create_eq_bars(lv_obj_t *parent, int y)
{
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_t *eq_container = lv_obj_create(parent);
    lv_obj_remove_style_all(eq_container);
    lv_obj_set_pos(eq_container, 0, y);
    lv_obj_set_size(eq_container, LCD_H_RES, MUSIC_EQ_H);
    lv_obj_clear_flag(eq_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(eq_container, LV_OPA_TRANSP, 0);

    lv_obj_t *eq_label = lv_label_create(eq_container);
    lv_label_set_text(eq_label, "BASS");
    lv_obj_set_style_text_color(eq_label, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(eq_label, lv_font_cn_get(14), 0);
    lv_obj_align(eq_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    lv_obj_t *treble_label = lv_label_create(eq_container);
    lv_label_set_text(treble_label, "TREBLE");
    lv_obj_set_style_text_color(treble_label, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(treble_label, lv_font_cn_get(14), 0);
    lv_obj_align(treble_label, LV_ALIGN_BOTTOM_RIGHT, -4, 0);

    int eq_bar_w = (LCD_H_RES - 24) / 4;
    for (int i = 0; i < 4; i++) {
        s_eq_bars[i] = lv_bar_create(eq_container);
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
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LCD_H_RES, MUSIC_HEADER_H);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);

    char status[64];
    if (s_is_playing && s_playing_idx >= 0) {
        char display_name[MUSIC_DISPLAY_LEN];
        music_get_display_name(s_music_entries[s_playing_idx], display_name, sizeof(display_name));
        snprintf(status, sizeof(status), "%s %s",
                 s_is_paused ? "⏸" : "▶", display_name);
    } else {
        snprintf(status, sizeof(status), "%s", lang_get(STR_MUSIC_LIST));
    }
    s_status_lbl = lv_label_create(header);
    lv_label_set_text(s_status_lbl, status);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(s_status_lbl, lv_font_cn_get(14), 0);
    lv_obj_set_width(s_status_lbl, LCD_H_RES - 60);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", s_volume);
    s_vol_lbl = lv_label_create(header);
    lv_label_set_text(s_vol_lbl, vol_str);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(s_vol_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ========== 区2：进度条 ========== */
    s_progress_bar = music_create_progress_bar(s_music_obj, MUSIC_HEADER_H + 2);

    /* ========== 区3：频谱 ========== */
    music_create_eq_bars(s_music_obj, MUSIC_HEADER_H + MUSIC_PROGRESS_H + 4);

    /* ========== 区4：分隔线 ========== */
    int divider_y = MUSIC_HEADER_H + MUSIC_PROGRESS_H + MUSIC_EQ_H + 6;
    lv_obj_t *divider = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(divider);
    lv_obj_set_pos(divider, 4, divider_y);
    lv_obj_set_size(divider, LCD_H_RES - 8, MUSIC_DIVIDER_H);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(divider, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);

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
        lv_label_set_text(empty_lbl, lang_get(STR_MUSIC_NO_FILE));
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(empty_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 10);
        return;
    }

    for (int i = 0; i < s_music_vis_rows; i++) {
        int idx = s_music_scroll + i;
        if (idx >= s_music_count) break;

        int row_y = list_y + i * s_music_row_h;
        lv_obj_t *row = lv_obj_create(s_music_obj);
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
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }

    /* ========== 区6：底部操作提示 ========== */
    int hint_y = list_y + s_music_vis_rows * s_music_row_h;
    if (hint_y + s_music_row_h < LCD_V_RES - DOCK_H) {
        lv_obj_t *hint_row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(hint_row);
        lv_obj_set_pos(hint_row, 0, hint_y);
        lv_obj_set_size(hint_row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(hint_row, LV_OPA_TRANSP, 0);
        lv_obj_t *hint_lbl = lv_label_create(hint_row);
        lv_label_set_text(hint_lbl, lang_get(STR_MUSIC_HINT));
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_CENTER, 0, 0);
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
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(title_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_width(title_lbl, LCD_H_RES - 8);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 2);
    y += font_px + 4;

    /* 歌词区（4行） */
    if (s_lrc_enabled && lrc_parser_is_loaded(&s_lrc)) {
        lv_obj_t *lrc_area = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(lrc_area);
        lv_obj_set_pos(lrc_area, 0, y);
        lv_obj_set_size(lrc_area, LCD_H_RES, MUSIC_LRC_LINES * (font_px + 1));
        lv_obj_clear_flag(lrc_area, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(lrc_area, LV_OPA_TRANSP, 0);

        for (int i = 0; i < MUSIC_LRC_LINES; i++) {
            lv_obj_t *lbl = lv_label_create(lrc_area);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(14), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_width(lbl, LCD_H_RES - 12);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_label_set_text(lbl, "");
            lv_obj_set_pos(lbl, 6, i * (font_px + 1));
            s_lrc_labels[i] = lbl;
        }
        y += MUSIC_LRC_LINES * (font_px + 1) + 2;
    } else {
        /* 无歌词时显示提示 */
        lv_obj_t *no_lrc = lv_label_create(s_music_obj);
        lv_label_set_text(no_lrc, lang_get(STR_MUSIC_LRC_OFF));
        lv_obj_set_style_text_color(no_lrc, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(no_lrc, lv_font_cn_get(14), 0);
        lv_obj_align(no_lrc, LV_ALIGN_TOP_MID, 0, y + 4);
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
             pos_s / 60, pos_s % 60, dur_s / 60, dur_s % 60);
    s_time_lbl = lv_label_create(s_music_obj);
    lv_label_set_text(s_time_lbl, time_str);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(s_time_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -4, y - MUSIC_PROGRESS_H / 2);
    y += font_px - 2;

    /* 循环模式 + 播放状态 */
    char mode_str[32];
    switch (s_play_mode) {
    case PLAY_MODE_SINGLE: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_SINGLE)); break;
    case PLAY_MODE_LIST_LOOP: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_LIST)); break;
    case PLAY_MODE_RANDOM: snprintf(mode_str, sizeof(mode_str), "%s", lang_get(STR_MUSIC_LOOP_RANDOM)); break;
    default: mode_str[0] = '\0'; break;
    }
    lv_obj_t *mode_lbl = lv_label_create(s_music_obj);
    lv_label_set_text(mode_lbl, mode_str);
    lv_obj_set_style_text_color(mode_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(mode_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(mode_lbl, LV_ALIGN_LEFT_MID, 4, y);

    /* 播放/暂停状态 */
    lv_obj_t *play_lbl = lv_label_create(s_music_obj);
    lv_label_set_text(play_lbl, s_is_paused ? lang_get(STR_MUSIC_PAUSE) : lang_get(STR_MUSIC_PLAY));
    lv_obj_set_style_text_color(play_lbl, lv_color_hex(colors->sel_bg), 0);
    lv_obj_set_style_text_font(play_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(play_lbl, LV_ALIGN_RIGHT_MID, -4, y);
    y += font_px + 4;

    /* 频谱（播放页） */
    if (s_eq_enabled) {
        music_create_eq_bars(s_music_obj, y);
        y += MUSIC_EQ_H + 2;
    }

    /* 底部操作提示 */
    lv_obj_t *hint_lbl = lv_label_create(s_music_obj);
    lv_label_set_text(hint_lbl, lang_get(STR_MUSIC_PLAY_HINT2));
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H);

    music_update_eq_visual();
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
    lv_label_set_text(title_lbl, lang_get(STR_MUSIC_SETTINGS));
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(title_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 4);

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

    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_music_obj);
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
        lv_label_set_text(lbl, labels[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *val_lbl = lv_label_create(row);
        lv_label_set_text(val_lbl, values[i]);
        lv_obj_set_style_text_color(val_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(val_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

        y += row_h;
    }

    /* 底部提示 */
    lv_obj_t *hint_lbl = lv_label_create(s_music_obj);
    lv_label_set_text(hint_lbl, lang_get(STR_MUSIC_SETTINGS_HINT));
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H);
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

    /* 从NVS加载音乐设置 */
    s_volume = audio_output_get_volume();
    s_eq_enabled = sys_nvs_load_music_eq() != 0;
    s_lrc_enabled = sys_nvs_load_music_lrc() != 0;
    int saved_mode = sys_nvs_load_music_mode();
    if (saved_mode >= 0 && saved_mode < PLAY_MODE_MAX) {
        s_play_mode = (play_mode_t)saved_mode;
    }

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_music_obj = list;

    s_music_sel = 0;
    s_music_scroll = 0;
    s_music_page = MUSIC_PAGE_LIST;

    music_scan_dir(s_music_current_path);
    music_refresh_page();

    if (s_ui_timer) lv_timer_del(s_ui_timer);
    s_ui_timer = lv_timer_create(music_ui_timer_cb, 200, NULL);

    ui_dock_create(scr, 1, 0);
}

static void music_destroy(void)
{
    ESP_LOGI(TAG, "Music destroy");
    music_stop_playback();
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
    s_music_count = 0;
    lrc_parser_clear(&s_lrc);
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
        if (key == KEY_B) {
            /* 返回列表页 */
            s_music_page = MUSIC_PAGE_LIST;
            music_refresh_page();
            return true;
        }
        if (key == KEY_A) {
            /* 播放/暂停 */
            if (s_is_playing) {
                s_is_paused = !s_is_paused;
                if (s_is_paused) {
                    audio_output_stop();
                }
                music_refresh_page();
            } else if (s_music_count > 0) {
                int idx = (s_playing_idx >= 0) ? s_playing_idx : s_music_sel;
                music_play_track(idx);
                music_refresh_page();
            }
            return true;
        }
        if (key == KEY_LEFT) {
            /* 上一首 */
            music_play_prev();
            music_refresh_page();
            return true;
        }
        if (key == KEY_RIGHT) {
            /* 下一首 */
            music_play_next();
            music_refresh_page();
            return true;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            /* 循环模式切换 */
            s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
            sys_nvs_save_music_mode((int)s_play_mode);
            music_refresh_page();
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
            case 1: /* 频谱开关 */
                s_eq_enabled = !s_eq_enabled;
                sys_nvs_save_music_eq(s_eq_enabled ? 1 : 0);
                music_refresh_page();
                break;
            case 2: /* 循环模式 */
                s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
                sys_nvs_save_music_mode((int)s_play_mode);
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
            case 1: /* 频谱开关 */
                s_eq_enabled = !s_eq_enabled;
                sys_nvs_save_music_eq(s_eq_enabled ? 1 : 0);
                music_refresh_page();
                break;
            case 2: /* 循环模式 */
                s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
                sys_nvs_save_music_mode((int)s_play_mode);
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
            }
            return true;
        }
        return true;
    }
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};