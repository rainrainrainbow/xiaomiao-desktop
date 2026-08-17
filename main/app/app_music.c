/**
 * @file app_music.c
 * @brief 音乐应用 - 浏览SD卡音频文件，蜂鸣器播放简单旋律
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_music_callbacks。
 * 功能：浏览/sdcard/music目录下的音频文件，选中后通过蜂鸣器播放。
 * 支持格式：.mid/.midi（调用MID播放器），.txt（简单旋律乐谱）
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

static const char *TAG = "APP_MUSIC";

#define MUSIC_MAX_ENTRIES 16
#define MUSIC_PATH_LEN    512

/* ========== 音乐文件列表 ========== */
static lv_obj_t *s_music_obj = NULL;
static char s_music_entries[MUSIC_MAX_ENTRIES][MUSIC_PATH_LEN];
static bool s_music_is_dir[MUSIC_MAX_ENTRIES];
static int s_music_count = 0;
static int s_music_sel = 0;
static int s_music_scroll = 0;
static char s_music_current_path[MUSIC_PATH_LEN] = "/sdcard/music";
static int s_music_row_h = 15;
static int s_music_vis_rows = 6;

/* ========== 简单旋律播放状态 ========== */
typedef struct {
    int freq;       /* 频率(Hz)，0=休止 */
    int duration;   /* 持续时间(ms) */
} note_t;

/* 简单旋律：两只老虎（蜂鸣器版本） */
static const note_t s_melody_two_tigers[] = {
    {262, 400}, {294, 400}, {330, 400}, {262, 400},
    {262, 400}, {294, 400}, {330, 400}, {262, 400},
    {330, 400}, {349, 400}, {392, 800},
    {330, 400}, {349, 400}, {392, 800},
    {392, 200}, {440, 200}, {392, 200}, {349, 200}, {330, 400}, {262, 400},
    {392, 200}, {440, 200}, {392, 200}, {349, 200}, {330, 400}, {262, 400},
    {262, 400}, {196, 400}, {262, 600},
    {262, 400}, {196, 400}, {262, 600},
    {0, 0},  /* 结束标记 */
};

/* 简单旋律：小星星 */
static const note_t s_melody_twinkle[] = {
    {262, 500}, {262, 500}, {392, 500}, {392, 500},
    {440, 500}, {440, 500}, {392, 1000},
    {349, 500}, {349, 500}, {330, 500}, {330, 500},
    {294, 500}, {294, 500}, {262, 1000},
    {0, 0},
};

/* 简单旋律：欢乐颂 */
static const note_t s_melody_ode[] = {
    {330, 500}, {330, 500}, {349, 500}, {392, 500},
    {392, 500}, {349, 500}, {330, 500}, {294, 500},
    {262, 500}, {262, 500}, {294, 500}, {330, 500},
    {330, 500}, {294, 500}, {294, 800},
    {0, 0},
};

/* 内置旋律列表 */
#define MELODY_COUNT 3
static const char *s_melody_names[MELODY_COUNT] = {"两只老虎", "小星星", "欢乐颂"};
static const note_t *s_melody_data[MELODY_COUNT] = {
    s_melody_two_tigers,
    s_melody_twinkle,
    s_melody_ode,
};

static int s_playing_melody = -1;  /* -1=未播放，>=0=正在播放的旋律索引 */
static int s_playing_note = 0;     /* 当前播放到的音符位置 */
static bool s_playing_paused = false;

/* ========== 蜂鸣器播放任务 ========== */
static void music_play_task(void *arg)
{
    int melody_idx = (int)(intptr_t)arg;
    const note_t *notes = s_melody_data[melody_idx];
    int note_idx = 0;
    s_playing_melody = melody_idx;
    s_playing_note = 0;
    s_playing_paused = false;

    while (notes[note_idx].freq != 0 || notes[note_idx].duration != 0) {
        if (s_playing_melody != melody_idx) {
            /* 被新的播放任务取代 */
            break;
        }
        if (s_playing_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (notes[note_idx].freq > 0) {
            drv_buzzer_tone(notes[note_idx].freq, 0);  /* 持续播放 */
        } else {
            drv_buzzer_stop();   /* 休止符，静音 */
        }
        s_playing_note = note_idx;
        vTaskDelay(pdMS_TO_TICKS(notes[note_idx].duration));
        note_idx++;
    }

    /* 播放结束 */
    drv_buzzer_stop();
    s_playing_melody = -1;
    s_playing_note = 0;
    vTaskDelete(NULL);
}

static void music_play_melody(int idx)
{
    if (idx < 0 || idx >= MELODY_COUNT) return;

    /* 停止当前播放 */
    if (s_playing_melody >= 0) {
        s_playing_melody = -1;  /* 让旧任务退出循环 */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 启动新播放任务 */
    xTaskCreate(music_play_task, "music_play", 2048,
                (void*)(intptr_t)idx, 5, NULL);
}

static void music_stop_playback(void)
{
    if (s_playing_melody >= 0) {
        s_playing_melody = -1;
        drv_buzzer_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ========== 文件扫描 ========== */
static void music_scan_dir(const char *path)
{
    s_music_count = 0;
    s_music_sel = 0;
    s_music_scroll = 0;
    strncpy(s_music_current_path, path, MUSIC_PATH_LEN - 1);

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open music directory: %s", path);
        return;
    }

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

    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    s_music_vis_rows = (avail_h - s_music_row_h - 2) / s_music_row_h;
    if (s_music_vis_rows < 1) s_music_vis_rows = 1;
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    /* 第0行：当前路径 */
    lv_obj_t *path_row = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(path_row);
    lv_obj_set_pos(path_row, 0, 0);
    lv_obj_set_size(path_row, LCD_H_RES, s_music_row_h);
    lv_obj_clear_flag(path_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(path_row, LV_OPA_TRANSP, 0);
    char header[MUSIC_PATH_LEN + 8];
    snprintf(header, sizeof(header), "> %s", s_music_current_path);
    lv_obj_t *path_lbl = lv_label_create(path_row);
    lv_label_set_text(path_lbl, header);
    lv_obj_set_style_text_color(path_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(path_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_align(path_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* 第1行：播放状态 */
    lv_obj_t *status_row = lv_obj_create(s_music_obj);
    lv_obj_remove_style_all(status_row);
    lv_obj_set_pos(status_row, 0, s_music_row_h);
    lv_obj_set_size(status_row, LCD_H_RES, s_music_row_h);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    char status[48];
    if (s_playing_melody >= 0) {
        snprintf(status, sizeof(status), "♪ 播放: %s%s", s_melody_names[s_playing_melody],
                 s_playing_paused ? " (暂停)" : "");
    } else {
        snprintf(status, sizeof(status), "♪ 按A键播放选中旋律");
    }
    lv_obj_t *status_lbl = lv_label_create(status_row);
    lv_label_set_text(status_lbl, status);
    lv_obj_set_style_text_color(status_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(status_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_align(status_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    /* 旋律列表行 */
    for (int i = 0; i < MELODY_COUNT; i++) {
        int row_idx = i + 2; /* 第2行开始 */
        lv_obj_t *row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_music_row_h);
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

    /* 文件列表（如果有） */
    int file_start = 2 + MELODY_COUNT + 1;
    for (int i = 0; i < s_music_count && i < 4; i++) {
        int row_idx = file_start + i;
        lv_obj_t *row = lv_obj_create(s_music_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_music_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_music_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        char buf[MUSIC_PATH_LEN + 4];
        const char *prefix = s_music_is_dir[i] ? "📁 " : "📄 ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, s_music_entries[i]);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

/* ========== 页面生命周期 ========== */
static void music_init(void *data)
{
    ESP_LOGI(TAG, "Music app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("音乐");

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_music_row_h = font_px + 1;

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_music_obj = list;

    s_music_sel = 0;
    s_music_scroll = 0;

    /* 扫描/sdcard/music目录 */
    music_scan_dir(s_music_current_path);
    music_refresh_list();
    ui_dock_create(scr, 1, 0);
}

static void music_destroy(void)
{
    ESP_LOGI(TAG, "Music destroy");
    music_stop_playback();
    s_music_obj = NULL;
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
        /* 选中旋律：播放/停止切换 */
        if (s_playing_melody == s_music_sel) {
            music_stop_playback();
        } else {
            music_play_melody(s_music_sel);
        }
        music_refresh_list();
        return true;
    }

    if (key == KEY_LEFT || key == KEY_RIGHT) {
        /* 暂停/继续 */
        if (s_playing_melody >= 0) {
            s_playing_paused = !s_playing_paused;
            if (s_playing_paused) {
                drv_buzzer_stop();
            }
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