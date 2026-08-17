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

/* ========== 播放状态变量 ========== */
static lv_obj_t *s_music_obj = NULL;
static lv_obj_t *s_progress_bar = NULL;   /* 播放进度条 */
static lv_obj_t *s_vis_indicator = NULL;  /* 音符可视化指示器 */
static lv_timer_t *s_ui_timer = NULL;     /* UI定时器，200ms刷新 */

static int s_music_sel = 0;               /* 当前选中旋律索引 */
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

/* ========== UI定时器回调（200ms刷新进度条+可视化） ========== */
static void music_ui_timer_cb(lv_timer_t *timer)
{
    if (!s_music_obj || !s_progress_bar) return;
    if (s_playing_melody >= 0 && s_total_notes > 0) {
        int pct = (s_playing_note * 100) / s_total_notes;
        if (pct > 100) pct = 100;
        lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
        if (s_vis_indicator) {
            if (s_current_freq > 0 && !s_playing_paused) {
                int vis_val = 8 + (s_current_freq - 130) * 92 / (1047 - 130);
                if (vis_val < 8) vis_val = 8;
                if (vis_val > 100) vis_val = 100;
                lv_bar_set_value(s_vis_indicator, vis_val, LV_ANIM_OFF);
                lv_obj_clear_flag(s_vis_indicator, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_bar_set_value(s_vis_indicator, 0, LV_ANIM_OFF);
                lv_obj_add_flag(s_vis_indicator, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        if (s_vis_indicator) {
            lv_bar_set_value(s_vis_indicator, 0, LV_ANIM_OFF);
            lv_obj_add_flag(s_vis_indicator, LV_OBJ_FLAG_HIDDEN);
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

/* ========== UI刷新 ========== */
static void music_refresh_list(void)
{
    if (!s_music_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_music_obj);
    s_progress_bar = NULL;
    s_vis_indicator = NULL;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_music_row_h = font_px + 1;

    /* 第0行：播放模式 + 状态 */
    char status[64];
    if (s_playing_melody >= 0) {
        snprintf(status, sizeof(status), "%s %s %s",
                 s_mode_icons[s_play_mode], s_melody_names[s_playing_melody],
                 s_playing_paused ? "⏸" : "▶");
    } else {
        snprintf(status, sizeof(status), "%s 按A播放", s_mode_icons[s_play_mode]);
    }
    lv_obj_t *row0 = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(row0);
    lv_obj_set_pos(row0, 0, 0);
    lv_obj_set_size(row0, LCD_H_RES, s_music_row_h);
    lv_obj_clear_flag(row0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row0, LV_OPA_TRANSP, 0);
    lv_obj_t *status_lbl = lv_label_create(row0);
    lv_label_set_text(status_lbl, status);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(status_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_align(status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* 第1行：进度条 */
    lv_obj_t *bar = lv_bar_create(s_music_obj);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
    lv_obj_set_size(bar, LCD_H_RES - 8, 6);
    lv_obj_set_pos(bar, 4, s_music_row_h + 2);
    lv_bar_set_range(bar, 0, 100);
    int pct = (s_playing_melody >= 0 && s_total_notes > 0) ? (s_playing_note * 100) / s_total_notes : 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    s_progress_bar = bar;

    /* 第2行：音符可视化指示器（小进度条，根据频率跳动） */
    lv_obj_t *vis = lv_bar_create(s_music_obj);
    lv_obj_remove_style_all(vis);
    lv_obj_set_style_bg_color(vis, lv_color_hex(colors->border), 0);
    lv_obj_set_style_bg_opa(vis, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(vis, 2, 0);
    lv_obj_set_style_bg_color(vis, lv_color_hex(colors->text), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(vis, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(vis, 2, LV_PART_INDICATOR);
    lv_obj_set_size(vis, LCD_H_RES - 8, 4);
    lv_obj_set_pos(vis, 4, s_music_row_h + 10);
    lv_bar_set_range(vis, 0, 100);
    lv_bar_set_value(vis, 0, LV_ANIM_OFF);
    if (!(s_playing_melody >= 0 && s_current_freq > 0 && !s_playing_paused)) {
        lv_obj_add_flag(vis, LV_OBJ_FLAG_HIDDEN);
    }
    s_vis_indicator = vis;

    /* 第3行开始：旋律列表 */
    int list_y = s_music_row_h + 16;
    for (int i = 0; i < MELODY_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, list_y + i * s_music_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_music_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        char buf[64];
        const char *play_icon = (i == s_playing_melody) ? "▶ " : "  ";
        snprintf(buf, sizeof(buf), "%s♪ %s", play_icon, s_melody_names[i]);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }

    /* 底部：操作提示 + 音量 */
    int hint_y = list_y + MELODY_COUNT * s_music_row_h;
    if (hint_y + s_music_row_h < LCD_V_RES - DOCK_H) {
        lv_obj_t *hint_row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(hint_row);
        lv_obj_set_pos(hint_row, 0, hint_y);
        lv_obj_set_size(hint_row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(hint_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(hint_row, LV_OPA_TRANSP, 0);
        char hint[64];
        snprintf(hint, sizeof(hint), "音量:%d %%  ←→调节", s_volume);
        lv_obj_t *hint_lbl = lv_label_create(hint_row);
        lv_label_set_text(hint_lbl, hint);
        lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(hint_lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }

    /* 文件列表（如果有） */
    int file_start = hint_y + 2;
    if (file_start + s_music_row_h < LCD_V_RES - DOCK_H) {
        for (int i = 0; i < s_music_count && i < 3; i++) {
            int row_y = file_start + i * s_music_row_h;
            if (row_y + s_music_row_h >= LCD_V_RES - DOCK_H) break;
            lv_obj_t *row = lv_obj_create(s_music_obj);
            lv_obj_remove_style_all(row);
            lv_obj_set_pos(row, 0, row_y);
            lv_obj_set_size(row, LCD_H_RES, s_music_row_h);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            char buf2[MUSIC_PATH_LEN + 4];
            const char *prefix = s_music_is_dir[i] ? "📁 " : "📄 ";
            snprintf(buf2, sizeof(buf2), "%s%s", prefix, s_music_entries[i]);
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, buf2);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
        }
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
    s_vis_indicator = NULL;
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