/**
 * @file app_music.c
 * @brief 音乐应用 - 蜂鸣器旋律播放器（改进版）
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_music_callbacks。
 * 功能：内置6首旋律，蜂鸣器播放，支持进度条、音符可视化、音量控制、播放模式。
 *
 * 操作说明：
 *   UP/DOWN  : 选择旋律
 *   LEFT/RIGHT: 调节音量
 *   A键      : 播放/暂停切换
 *   长按A    : 停止播放
 *   B键      : 返回上一级
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "driver/drv_buzzer.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
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

#define MUSIC_MAX_ENTRIES 16
#define MUSIC_PATH_LEN    512

/* ========== 旋律数据结构 ========== */
typedef struct {
    int freq;       /* 频率(Hz)，0=休止 */
    int duration;   /* 持续时间(ms) */
} note_t;

/* ========== 播放模式 ========== */
typedef enum {
    PLAY_MODE_SINGLE = 0,      /* 单曲播放（播完即止） */
    PLAY_MODE_SINGLE_LOOP,     /* 单曲循环 */
    PLAY_MODE_LIST_LOOP,       /* 列表循环 */
    PLAY_MODE_RANDOM,          /* 随机播放 */
    PLAY_MODE_MAX
} play_mode_t;

static const char *s_mode_icons[PLAY_MODE_MAX] = {
    "▶", "🔁", "🔂", "🔀"
};

/* ========== 内置旋律（6首） ========== */

/* 1. 两只老虎 */
static const note_t s_melody_two_tigers[] = {
    {262, 400}, {294, 400}, {330, 400}, {262, 400},
    {262, 400}, {294, 400}, {330, 400}, {262, 400},
    {330, 400}, {349, 400}, {392, 800},
    {330, 400}, {349, 400}, {392, 800},
    {392, 200}, {440, 200}, {392, 200}, {349, 200}, {330, 400}, {262, 400},
    {392, 200}, {440, 200}, {392, 200}, {349, 200}, {330, 400}, {262, 400},
    {262, 400}, {196, 400}, {262, 600},
    {262, 400}, {196, 400}, {262, 600},
    {0, 0},
};

/* 2. 小星星 */
static const note_t s_melody_twinkle[] = {
    {262, 500}, {262, 500}, {392, 500}, {392, 500},
    {440, 500}, {440, 500}, {392, 1000},
    {349, 500}, {349, 500}, {330, 500}, {330, 500},
    {294, 500}, {294, 500}, {262, 1000},
    {0, 0},
};

/* 3. 欢乐颂 */
static const note_t s_melody_ode[] = {
    {330, 500}, {330, 500}, {349, 500}, {392, 500},
    {392, 500}, {349, 500}, {330, 500}, {294, 500},
    {262, 500}, {262, 500}, {294, 500}, {330, 500},
    {330, 500}, {294, 500}, {294, 800},
    {0, 0},
};

/* 4. 生日快乐 */
static const note_t s_melody_birthday[] = {
    {262, 400}, {262, 400}, {294, 800}, {262, 800},
    {349, 800}, {330, 1200},
    {262, 400}, {262, 400}, {294, 800}, {262, 800},
    {392, 800}, {349, 1200},
    {262, 400}, {262, 400}, {523, 800}, {440, 800},
    {349, 800}, {330, 800}, {294, 800},
    {494, 400}, {494, 400}, {440, 800}, {349, 800},
    {392, 800}, {349, 1200},
    {0, 0},
};

/* 5. 茉莉花 */
static const note_t s_melody_jasmine[] = {
    {330, 400}, {392, 400}, {440, 400}, {523, 400},
    {523, 400}, {440, 400}, {392, 800},
    {330, 400}, {392, 400}, {440, 400}, {523, 400},
    {523, 400}, {440, 400}, {392, 800},
    {392, 400}, {440, 400}, {523, 400}, {587, 400},
    {587, 400}, {523, 400}, {440, 400}, {392, 400},
    {330, 400}, {294, 400}, {330, 800},
    {0, 0},
};

/* 6. 致爱丽丝（简单版片段） */
static const note_t s_melody_elise[] = {
    {330, 300}, {330, 300}, {349, 300}, {392, 300},
    {440, 300}, {523, 500}, {440, 300}, {392, 300},
    {349, 300}, {330, 300}, {294, 300}, {262, 300},
    {330, 300}, {330, 300}, {349, 300}, {392, 300},
    {440, 300}, {523, 500}, {440, 300}, {392, 300},
    {349, 300}, {330, 300}, {294, 300}, {262, 300},
    {0, 0},
};

#define MELODY_COUNT 6
static const char *s_melody_names[MELODY_COUNT] = {
    "两只老虎", "小星星", "欢乐颂", "生日快乐", "茉莉花", "致爱丽丝"
};
static const note_t *s_melody_data[MELODY_COUNT] = {
    s_melody_two_tigers, s_melody_twinkle, s_melody_ode,
    s_melody_birthday,   s_melody_jasmine, s_melody_elise
};

/* ========== UI布局常量（160x128屏，内容区约108px高） ========== */
#define MUSIC_HEADER_H     18   /* 顶部卡带装饰区高度 */
#define MUSIC_PROGRESS_H   6    /* 进度条高度 */
#define MUSIC_EQ_H         14   /* 均衡器可视化区高度 */
#define MUSIC_DIVIDER_H    2    /* 分隔线高度 */
#define MUSIC_LIST_TOP     (MUSIC_HEADER_H + MUSIC_PROGRESS_H + 2 + MUSIC_EQ_H + 2 + MUSIC_DIVIDER_H)  /* 44 */

/* ========== UI对象变量 ========== */
static lv_obj_t *s_music_obj = NULL;
static lv_obj_t *s_progress_bar = NULL;   /* 播放进度条 */
static lv_obj_t *s_eq_bars[4] = {NULL};   /* 4段均衡器条 */
static lv_obj_t *s_status_lbl = NULL;     /* 状态文字（当前旋律名） */
static lv_obj_t *s_vol_lbl = NULL;        /* 音量标签 */
static lv_timer_t *s_ui_timer = NULL;     /* UI定时器，200ms刷新 */

static int s_music_sel = 0;               /* 当前选中旋律索引 */
static int s_music_scroll = 0;            /* 列表滚动偏移 */
static int s_music_vis_rows = 0;          /* 可见行数 */
static int s_playing_melody = -1;         /* -1=未播放，>=0=正在播放的旋律索引 */
static int s_playing_note = 0;            /* 当前播放到的音符位置 */
static int s_total_notes = 0;             /* 当前旋律总音符数 */
static bool s_playing_paused = false;
static int s_current_freq = 0;            /* 当前播放频率，用于可视化 */
static play_mode_t s_play_mode = PLAY_MODE_SINGLE;
static int s_volume = 50;                 /* 音量 0-100 */

/* 文件扫描相关（保留SD卡目录浏览功能） */
static char s_music_entries[MUSIC_MAX_ENTRIES][MUSIC_PATH_LEN];
static bool s_music_is_dir[MUSIC_MAX_ENTRIES];
static int s_music_count = 0;
static char s_music_current_path[MUSIC_PATH_LEN] = "/sdcard/music";
static int s_music_row_h = 15;

/* ========== 蜂鸣器播放任务 ========== */
static void music_play_task(void *arg)
{
    int melody_idx = (int)(intptr_t)arg;
    s_playing_melody = melody_idx;
    s_playing_note = 0;
    s_playing_paused = false;
    s_current_freq = 0;

    /* 计算总音符数 */
    const note_t *notes = s_melody_data[melody_idx];
    int total = 0;
    while (notes[total].freq != 0 || notes[total].duration != 0) total++;
    s_total_notes = total;

    int note_idx = 0;
    const note_t *current_notes = notes;

    while (1) {
        if (s_playing_melody != melody_idx) break;

        if (note_idx >= total) {
            /* 播放结束，根据播放模式决定下一步 */
            if (s_play_mode == PLAY_MODE_SINGLE) {
                drv_buzzer_stop();
                s_playing_melody = -1;
                s_playing_note = 0;
                s_total_notes = 0;
                s_current_freq = 0;
                vTaskDelete(NULL);
                return;
            } else if (s_play_mode == PLAY_MODE_SINGLE_LOOP) {
                note_idx = 0;
                s_playing_note = 0;
                continue;
            } else {
                /* 列表循环或随机播放 */
                if (s_play_mode == PLAY_MODE_LIST_LOOP) {
                    melody_idx = (melody_idx + 1) % MELODY_COUNT;
                } else {
                    int next = rand() % MELODY_COUNT;
                    if (next == melody_idx && MELODY_COUNT > 1)
                        next = (next + 1) % MELODY_COUNT;
                    melody_idx = next;
                }
                s_playing_melody = melody_idx;
                note_idx = 0;
                s_playing_note = 0;
                current_notes = s_melody_data[melody_idx];
                total = 0;
                while (current_notes[total].freq != 0 || current_notes[total].duration != 0) total++;
                s_total_notes = total;
                continue;
            }
        }

        if (s_playing_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (current_notes[note_idx].freq > 0) {
            drv_buzzer_tone(current_notes[note_idx].freq, 0);
            s_current_freq = current_notes[note_idx].freq;
        } else {
            drv_buzzer_stop();
            s_current_freq = 0;
        }
        s_playing_note = note_idx;
        vTaskDelay(pdMS_TO_TICKS(current_notes[note_idx].duration));
        note_idx++;
    }

    /* 被取代，清理 */
    drv_buzzer_stop();
    s_playing_melody = -1;
    s_playing_note = 0;
    s_total_notes = 0;
    s_current_freq = 0;
    vTaskDelete(NULL);
}

static void music_start_playback(int idx)
{
    if (idx < 0 || idx >= MELODY_COUNT) return;
    if (s_playing_melody >= 0) {
        s_playing_melody = -1;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_playing_note = 0;
    s_total_notes = 0;
    s_current_freq = 0;
    xTaskCreate(music_play_task, "music_play", 2048,
                (void*)(intptr_t)idx, 5, NULL);
}

static void music_stop_playback(void)
{
    if (s_playing_melody >= 0) {
        s_playing_melody = -1;
        drv_buzzer_stop();
        s_playing_note = 0;
        s_total_notes = 0;
        s_current_freq = 0;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ========== UI定时器回调（200ms刷新进度条+均衡器+转轮+状态文字） ========== */
static void music_ui_timer_cb(lv_timer_t *timer)
{
    if (!s_music_obj || !s_progress_bar) return;

    if (s_playing_melody >= 0 && s_total_notes > 0) {
        /* 更新进度条 */
        int pct = (s_playing_note * 100) / s_total_notes;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);

        /* 更新4段均衡器：根据频率拆分到4个频段模拟多重频段效果 */
        if (s_current_freq > 0 && !s_playing_paused) {
            /* 4个频段：低(130-262) 中低(262-440) 中高(440-698) 高(698-1047) */
            int freq = s_current_freq;
            int vals[4] = {0};
            if (freq <= 262) {
                vals[0] = 30 + (freq - 130) * 70 / (262 - 130);  /* 低音最强 */
                vals[1] = vals[0] * 2 / 3;
                vals[2] = vals[0] / 3;
                vals[3] = 5;
            } else if (freq <= 440) {
                vals[1] = 30 + (freq - 262) * 70 / (440 - 262);
                vals[0] = vals[1] * 2 / 3;
                vals[2] = vals[1] / 2;
                vals[3] = 10;
            } else if (freq <= 698) {
                vals[2] = 30 + (freq - 440) * 70 / (698 - 440);
                vals[1] = vals[2] * 2 / 3;
                vals[3] = vals[2] / 2;
                vals[0] = 10;
            } else {
                vals[3] = 30 + (freq - 698) * 70 / (1047 - 698);
                vals[2] = vals[3] * 2 / 3;
                vals[1] = vals[3] / 3;
                vals[0] = 5;
            }
            for (int i = 0; i < 4; i++) {
                if (vals[i] > 100) vals[i] = 100;
                if (vals[i] < 0) vals[i] = 0;
                if (s_eq_bars[i]) {
                    lv_bar_set_value(s_eq_bars[i], vals[i], LV_ANIM_OFF);
                    lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            for (int i = 0; i < 4; i++) {
                if (s_eq_bars[i]) {
                    lv_bar_set_value(s_eq_bars[i], 0, LV_ANIM_OFF);
                    if (s_playing_paused) {
                        /* 暂停时显示为低电平抖动 */
                        lv_bar_set_value(s_eq_bars[i], 5 + (i * 3), LV_ANIM_OFF);
                    }
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

/* ========== 文件扫描 ========== */
static void music_scan_dir(const char *path)
{
    s_music_count = 0;
    strncpy(s_music_current_path, path, MUSIC_PATH_LEN - 1);
    DIR *dir = opendir(path);
    if (!dir) { ESP_LOGW(TAG, "Cannot open music directory: %s", path); return; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_music_count < MUSIC_MAX_ENTRIES) {
        if (entry->d_name[0] == '.') continue;
        strncpy(s_music_entries[s_music_count], entry->d_name, MUSIC_PATH_LEN - 1);
        s_music_entries[s_music_count][MUSIC_PATH_LEN - 1] = '\0';
        s_music_is_dir[s_music_count] = (entry->d_type == DT_DIR);
        s_music_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Music scan: %d entries in %s", s_music_count, path);
}

/* ========== UI刷新：复古卡带随身听风格 ==========
 * 布局（从上到下，内容区约108px高）：
 *   ┌──────────────────────────────────┐
 *   │  🎵 小星星          ▶  🔁  │  ← 顶部状态行（18px）
 *   │  ════════════════               │  ← 进度条（6px）
 *   │  ██ ██ ██ ██                    │  ← 4段均衡器（14px）
 *   │  ──────────────────              │  ← 分隔线（2px）
 *   │  01 两只老虎         ▶  │  ← 歌单列表（可滚动，选中高亮）
 *   │  02 小星星                     │
 *   │  03 欢乐颂                      │
 *   │  ...                            │
 *   │  音量: 50%  ←→            │  ← 底部提示
 *   └──────────────────────────────────┘
 */
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

    /* ========== 区1：顶部卡带状态区（18px） ==========
     * 左侧：模式图标 + 当前旋律/状态
     * 右侧：播放/暂停图标
     */
    lv_obj_t *header = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LCD_H_RES, MUSIC_HEADER_H);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);

    /* 左侧：模式图标 + 旋律名 */
    char status[64];
    if (s_playing_melody >= 0) {
        snprintf(status, sizeof(status), "%s%s%s",
                 s_mode_icons[s_play_mode],
                 s_melody_names[s_playing_melody],
                 s_playing_paused ? " ⏸" : " ▶");
    } else {
        snprintf(status, sizeof(status), "%s 按A播放", s_mode_icons[s_play_mode]);
    }
    s_status_lbl = lv_label_create(header);
    lv_label_set_text(s_status_lbl, status);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(s_status_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* 右侧：音量指示（小条） */
    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", s_volume);
    s_vol_lbl = lv_label_create(header);
    lv_label_set_text(s_vol_lbl, vol_str);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(s_vol_lbl, lv_font_cn_get(14), 0);
    lv_obj_align(s_vol_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

    /* 音量小进度条 */
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

    /* ========== 区2：进度条（6px） ========== */
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
    int pct = (s_playing_melody >= 0 && s_total_notes > 0) ? (s_playing_note * 100) / s_total_notes : 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    s_progress_bar = bar;

    /* ========== 区3：4段均衡器可视化（14px） ==========
     * 4个等宽竖条，从左到右：低音→高音
     * 每个条宽约34px，间距4px
     */
    int eq_y = MUSIC_HEADER_H + MUSIC_PROGRESS_H + 4;
    lv_obj_t *eq_container = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(eq_container);
    lv_obj_set_pos(eq_container, 0, eq_y);
    lv_obj_set_size(eq_container, LCD_H_RES, MUSIC_EQ_H);
    lv_obj_clear_flag(eq_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(eq_container, LV_OPA_TRANSP, 0);

    /* 均衡器标签（左下角小字：BASS / TREBLE） */
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

    int eq_bar_w = (LCD_H_RES - 24) / 4;  /* 约34px */
    for (int i = 0; i < 4; i++) {
        s_eq_bars[i] = lv_bar_create(eq_container);
        lv_obj_remove_style_all(s_eq_bars[i]);
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(colors->border), 0);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_eq_bars[i], 1, 0);
        /* 不同频段用不同颜色：低音用text色，高音用sel_bg色 */
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

    /* ========== 区4：分隔线（2px） ========== */
    int divider_y = eq_y + MUSIC_EQ_H + 2;
    lv_obj_t *divider = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(divider);
    lv_obj_set_pos(divider, 4, divider_y);
    lv_obj_set_size(divider, LCD_H_RES - 8, MUSIC_DIVIDER_H);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(divider, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_20, 0);

    /* ========== 区5：旋律列表（可滚动，选中高亮） ========== */
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

    /* 确保选中项可见 */
    if (s_music_sel < s_music_scroll) s_music_scroll = s_music_sel;
    if (s_music_sel >= s_music_scroll + s_music_vis_rows) s_music_scroll = s_music_sel - s_music_vis_rows + 1;
    if (s_music_scroll < 0) s_music_scroll = 0;
    if (s_music_scroll > MELODY_COUNT - s_music_vis_rows) s_music_scroll = MELODY_COUNT - s_music_vis_rows;
    if (s_music_scroll < 0) s_music_scroll = 0;

    for (int i = 0; i < s_music_vis_rows; i++) {
        int idx = s_music_scroll + i;
        if (idx >= MELODY_COUNT) break;

        int row_y = list_y + i * s_music_row_h;
        lv_obj_t *row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_y);
        lv_obj_set_size(row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (idx == s_music_sel) {
            /* 选中项：棕色背景 + 黑色文字 */
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        /* 编号 + 歌曲名 + 播放指示 */
        char buf[64];
        const char *playing_mark = (idx == s_playing_melody) ? "▶" : " ";
        snprintf(buf, sizeof(buf), "%02d %s", idx + 1, s_melody_names[idx]);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        /* 右侧：播放标记 */
        if (idx == s_playing_melody) {
            lv_obj_t *play_lbl = lv_label_create(row);
            lv_label_set_text(play_lbl, playing_mark);
            lv_obj_set_style_text_color(play_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_style_text_font(play_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_align(play_lbl, LV_ALIGN_RIGHT_MID, -6, 0);
        }
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
        lv_label_set_text(hint_lbl, "←→音量  A播放  B返回");
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(14), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_CENTER, 0, 0);
    }
}

/* ========== 页面生命周期 ========== */
static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("音乐");

    s_volume = drv_buzzer_get_volume();

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

    /* 创建UI定时器（200ms刷新） */
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
        s_music_sel = (s_music_sel - 1 + MELODY_COUNT) % MELODY_COUNT;
        music_refresh_list();
        return true;
    }

    if (key == KEY_DOWN) {
        s_music_sel = (s_music_sel + 1) % MELODY_COUNT;
        music_refresh_list();
        return true;
    }

    if (key == KEY_A) {
        if (s_playing_melody == s_music_sel) {
            /* 已经选中正在播放的旋律：暂停/继续交替 */
            if (s_playing_melody >= 0) {
                s_playing_paused = !s_playing_paused;
                if (s_playing_paused) drv_buzzer_stop();
                music_refresh_list();
            }
        } else {
            music_start_playback(s_music_sel);
            music_refresh_list();
        }
        return true;
    }

    if (key == KEY_LEFT) {
        /* 左键：调节音量（减小） */
        s_volume -= 10;
        if (s_volume < 0) s_volume = 0;
        drv_buzzer_set_volume(s_volume);
        music_refresh_list();
        return true;
    }

    if (key == KEY_RIGHT) {
        /* 右键：调节音量（增大）/ 切换播放模式 */
        if (s_playing_melody >= 0) {
            /* 播放中：切换播放模式 */
            s_play_mode = (play_mode_t)((s_play_mode + 1) % PLAY_MODE_MAX);
            if (s_play_mode == PLAY_MODE_RANDOM) {
                srand((unsigned)esp_log_timestamp());
            }
            music_refresh_list();
        } else {
            s_volume += 10;
            if (s_volume > 100) s_volume = 100;
            drv_buzzer_set_volume(s_volume);
            music_refresh_list();
        }
        return true;
    }

    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_music_callbacks = {
    .init = music_init,
    .destroy = music_destroy,
    .on_key = music_on_key,
};