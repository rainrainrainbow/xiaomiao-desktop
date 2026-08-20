/**
 * @file app_music.c
 * @brief 音乐应用 - 真实音频播放器（WAV文件播放）
 *
 * 架构说明：
 * - 使用 drv_audio_decoder 解码WAV文件
 * - 使用 drv_audio_output 播放PCM数据（通过I2S DAC或蓝牙A2DP）
 * - 独立播放任务避免阻塞UI
 * - 扫描/sdcard/music目录下的.wav文件
 *
 * 操作说明：
 *   UP/DOWN  : 选择文件
 *   LEFT     : 音量-10%
 *   RIGHT    : 播放模式切换（单曲/列表循环/随机）
 *   A键      : 播放/暂停切换
 *   长按A    : 停止播放
 *   B键      : 返回上一级
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "driver/drv_audio_output.h"
#include "driver/drv_audio_decoder.h"
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
    PLAY_MODE_SINGLE = 0,      /* 单曲播放（播完即止） */
    PLAY_MODE_LIST_LOOP,       /* 列表循环 */
    PLAY_MODE_RANDOM,          /* 随机播放 */
    PLAY_MODE_MAX
} play_mode_t;

static const char *s_mode_icons[PLAY_MODE_MAX] = {
    "▶", "🔂", "🔀"
};

/* ========== UI布局常量 ========== */
#define MUSIC_HEADER_H     18   /* 顶部状态区高度 */
#define MUSIC_PROGRESS_H   6    /* 进度条高度 */
#define MUSIC_EQ_H         14   /* 均衡器可视化区高度 */
#define MUSIC_DIVIDER_H    2    /* 分隔线高度 */
#define MUSIC_LIST_TOP     (MUSIC_HEADER_H + MUSIC_PROGRESS_H + 2 + MUSIC_EQ_H + 2 + MUSIC_DIVIDER_H)

/* ========== UI对象变量 ========== */
static lv_obj_t *s_music_obj = NULL;
static lv_obj_t *s_progress_bar = NULL;   /* 播放进度条 */
static lv_obj_t *s_eq_bars[4] = {NULL};   /* 4段均衡器条 */
static lv_obj_t *s_status_lbl = NULL;     /* 状态文字 */
static lv_obj_t *s_vol_lbl = NULL;        /* 音量标签 */
static lv_timer_t *s_ui_timer = NULL;     /* UI定时器，200ms刷新 */

static int s_music_sel = 0;               /* 当前选中索引 */
static int s_music_scroll = 0;            /* 列表滚动偏移 */
static int s_music_vis_rows = 0;          /* 可见行数 */
static int s_music_row_h = 15;

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

/* 解码器（播放任务使用） */
static wav_decoder_t s_wav_decoder;
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

        /* 只添加.wav文件 */
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

/* ========== 播放任务 ========== */
static void music_play_task(void *arg)
{
    int file_idx = (int)(intptr_t)arg;
    char fullpath[MUSIC_FULLPATH_LEN];

    snprintf(fullpath, sizeof(fullpath), "%s/%s",
             s_music_current_path, s_music_entries[file_idx]);

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
                if (s_play_mode == PLAY_MODE_SINGLE) {
                    /* 单曲播放完毕 */
                    break;
                } else if (s_play_mode == PLAY_MODE_LIST_LOOP) {
                    /* 列表循环：播放下一个 */
                    file_idx = (file_idx + 1) % s_music_count;
                } else if (s_play_mode == PLAY_MODE_RANDOM) {
                    /* 随机播放 */
                    int next = rand() % s_music_count;
                    if (next == file_idx && s_music_count > 1)
                        next = (next + 1) % s_music_count;
                    file_idx = next;
                }

                s_playing_idx = file_idx;

                /* 关闭当前文件，打开下一个 */
                wav_decoder_close(&s_wav_decoder);

                snprintf(fullpath, sizeof(fullpath), "%s/%s",
                         s_music_current_path, s_music_entries[file_idx]);

                if (wav_decoder_open(&s_wav_decoder, fullpath) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to open next file: %s", fullpath);
                    break;
                }

                const wav_format_t *next_fmt = wav_decoder_get_format(&s_wav_decoder);
                audio_output_open(next_fmt->sample_rate, 16, next_fmt->channels);
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

/* ========== UI定时器回调（200ms刷新进度条+均衡器） ========== */
static void music_ui_timer_cb(lv_timer_t *timer)
{
    if (!s_music_obj || !s_progress_bar) return;

    if (s_is_playing && s_playing_idx >= 0) {
        uint32_t pos_ms = wav_decoder_get_position_ms(&s_wav_decoder);
        const wav_format_t *fmt = wav_decoder_get_format(&s_wav_decoder);
        uint32_t dur_ms = fmt ? fmt->duration_ms : 0;

        if (dur_ms > 0) {
            int pct = (int)(pos_ms * 100 / dur_ms);
            if (pct > 100) pct = 100;
            lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
        }

        if (!s_is_paused) {
            for (int i = 0; i < 4; i++) {
                int val = 20 + (rand() % 60);
                if (s_eq_bars[i]) {
                    lv_bar_set_value(s_eq_bars[i], val, LV_ANIM_OFF);
                    lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    } else {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        for (int i = 0; i < 4; i++) {
            if (s_eq_bars[i]) {
                lv_bar_set_value(s_eq_bars[i], 0, LV_ANIM_OFF);
                lv_obj_add_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* ========== UI刷新 ========== */
static void music_refresh_list(void)
{
    if (!s_music_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_music_obj);
    s_progress_bar = NULL;
    for (int i = 0; i < 4; i++) s_eq_bars[i] = NULL;
    s_status_lbl = NULL;
    s_vol_lbl = NULL;

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
        const char *name = s_music_entries[s_playing_idx];
        char display_name[32];
        strncpy(display_name, name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
        char *dot = strrchr(display_name, '.');
        if (dot) *dot = '\0';

        snprintf(status, sizeof(status), "%s%s%s",
                 s_mode_icons[s_play_mode],
                 display_name,
                 s_is_paused ? " ⏸" : " ▶");
    } else {
        snprintf(status, sizeof(status), "%s %s", s_mode_icons[s_play_mode], lang_get(STR_MUSIC_PLAY_HINT));
    }
    s_status_lbl = lv_label_create(header);
    lv_label_set_text(s_status_lbl, status);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(s_status_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", s_volume);
    s_vol_lbl = lv_label_create(header);
    lv_label_set_text(s_vol_lbl, vol_str);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(s_vol_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    lv_obj_t *vol_bar = lv_bar_create(header);
    lv_obj_remove_style_all(vol_bar);
    lv_obj_set_style_bg_color(vol_bar, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(vol_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vol_bar, 1, 0);
    lv_obj_set_style_bg_color(vol_bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vol_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(vol_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_size(vol_bar, 24, 4);
    lv_bar_set_range(vol_bar, 0, 100);
    lv_bar_set_value(vol_bar, s_volume, LV_ANIM_OFF);
    lv_obj_align(vol_bar, LV_ALIGN_RIGHT_MID, -32, 0);

    /* ========== 区2：进度条 ========== */
    lv_obj_t *bar = lv_bar_create(s_music_obj);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_size(bar, LCD_H_RES - 8, MUSIC_PROGRESS_H);
    lv_obj_set_pos(bar, 4, MUSIC_HEADER_H + 2);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    s_progress_bar = bar;

    /* ========== 区3：4段均衡器可视化 ========== */
    int eq_y = MUSIC_HEADER_H + MUSIC_PROGRESS_H + 4;
    lv_obj_t *eq_container = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(eq_container);
    lv_obj_set_pos(eq_container, 0, eq_y);
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

    /* ========== 区4：分隔线 ========== */
    int divider_y = eq_y + MUSIC_EQ_H + 2;
    lv_obj_t *divider = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(divider);
    lv_obj_set_pos(divider, 4, divider_y);
    lv_obj_set_size(divider, LCD_H_RES - 8, MUSIC_DIVIDER_H);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(divider, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);

    /* ========== 区5：WAV文件列表 ========== */
    int list_y = MUSIC_LIST_TOP;
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H - list_y;
    if (font_px >= 20) {
        s_music_vis_rows = avail_h / s_music_row_h;
        if (s_music_vis_rows > 4) s_music_vis_rows = 4;
    } else {
        s_music_vis_rows = avail_h / s_music_row_h;
        if (s_music_vis_rows > 5) s_music_vis_rows = 5;
    }
    if (s_music_vis_rows < 1) s_music_vis_rows = 1;

    if (s_music_sel < s_music_scroll) s_music_scroll = s_music_sel;
    if (s_music_sel >= s_music_scroll + s_music_vis_rows)
        s_music_scroll = s_music_sel - s_music_vis_rows + 1;
    if (s_music_scroll < 0) s_music_scroll = 0;
    if (s_music_scroll > s_music_count - s_music_vis_rows)
        s_music_scroll = s_music_count - s_music_vis_rows;
    if (s_music_scroll < 0) s_music_scroll = 0;

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

        char buf[64];
        const char *name = s_music_entries[idx];
        char display_name[MUSIC_DISPLAY_LEN];
        strncpy(display_name, name, sizeof(display_name) - 1);
        display_name[sizeof(display_name) - 1] = '\0';
        char *dot = strrchr(display_name, '.');
        if (dot) *dot = '\0';

        const char *playing_mark = (idx == s_playing_idx && s_is_playing) ? "▶" : " ";
        snprintf(buf, sizeof(buf), "%s %.28s", playing_mark, display_name);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
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
}

/* ========== 页面生命周期 ========== */
static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music app init (WAV player)");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_MUSIC));

    s_volume = audio_output_get_volume();

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_music_obj = list;

    s_music_sel = 0;
    s_music_scroll = 0;

    music_scan_dir(s_music_current_path);
    music_refresh_list();

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
    s_music_count = 0;
}

static bool music_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (key == KEY_UP) {
        if (s_music_count > 0) {
            s_music_sel = (s_music_sel - 1 + s_music_count) % s_music_count;
            music_refresh_list();
        }
        return true;
    }

    if (key == KEY_DOWN) {
        if (s_music_count > 0) {
            s_music_sel = (s_music_sel + 1) % s_music_count;
            music_refresh_list();
        }
        return true;
    }

    if (key == KEY_A) {
        if (s_music_count <= 0) return true;

        if (s_is_playing && s_playing_idx == s_music_sel) {
            s_is_paused = !s_is_paused;
            if (s_is_paused) {
                audio_output_stop();
            }
            music_refresh_list();
        } else {
            music_start_playback(s_music_sel);
            music_refresh_list();
        }
        return true;
    }

    if (key == KEY_LEFT) {
        s_volume -= 10;
        if (s_volume < 0) s_volume = 0;
        audio_output_set_volume(s_volume);
        sys_nvs_save_volume(s_volume);
        music_refresh_list();
        return true;
    }

    if (key == KEY_RIGHT) {
        if (s_is_playing) {
            s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
            if (s_play_mode == PLAY_MODE_RANDOM) {
                srand((unsigned)(esp_timer_get_time() / 1000));
            }
            music_refresh_list();
        } else {
            s_volume += 10;
            if (s_volume > 100) s_volume = 100;
            audio_output_set_volume(s_volume);
            sys_nvs_save_volume(s_volume);
            music_refresh_list();
        }
        return true;
    }

    return true;
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};