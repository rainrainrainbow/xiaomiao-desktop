/**
 * @file app_midplayer.c
 * @brief MID蜂鸣器播放器应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_midplayer_callbacks。
 * 功能：解析标准MIDI文件（SMF格式0/1），用蜂鸣器播放音符。
 * 从文件管理器通过 stash 传递 .mid 文件路径打开。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "driver/drv_buzzer.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "APP_MIDPLAYER";

#define MID_MAX_NOTES   512   /* 最多解析的音符事件数 */
#define MID_MAX_FILE    65536 /* 最大MIDI文件大小 64KB */

/* 音符事件（解析后展开） */
typedef struct {
    int note;          /* MIDI音符编号 0-127 */
    uint32_t duration; /* 持续时间（ms） */
} mid_note_t;

/* 播放器状态 */
typedef enum {
    MID_STATE_IDLE = 0,
    MID_STATE_PLAYING,
    MID_STATE_PAUSED,
    MID_STATE_DONE,
} mid_state_t;

static lv_obj_t *s_mid_obj = NULL;
static lv_obj_t *s_mid_bar = NULL;   /* 播放进度条 */
static mid_note_t s_mid_notes[MID_MAX_NOTES];
static int s_mid_note_count = 0;
static int s_mid_play_pos = 0;
static mid_state_t s_mid_state = MID_STATE_IDLE;
static char s_mid_file_path[256] = "";
static char s_mid_file_name[64] = "";

/* ========== MIDI 文件解析 ========== */

/* 读取可变长度数值（MIDI delta-time 格式） */
static uint32_t mid_read_varlen(const uint8_t *data, int size, int *pos)
{
    uint32_t value = 0;
    for (int i = 0; i < 4 && *pos < size; i++) {
        uint8_t b = data[*pos];
        (*pos)++;
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

/* 解析MIDI文件，提取音符事件序列 */
static int mid_parse(const uint8_t *data, int size)
{
    if (size < 14) return -1;
    /* 检查文件头 MThd */
    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd')
        return -1;

    int format = (data[8] << 8) | data[9];
    int num_tracks = (data[10] << 8) | data[11];
    int division = (data[12] << 8) | data[13];
    ESP_LOGI(TAG, "MIDI format=%d tracks=%d division=%d", format, num_tracks, division);

    /* 时基：每四分音符的tick数（常见 480/960） */
    int ticks_per_quarter = division;
    if (ticks_per_quarter <= 0) ticks_per_quarter = 480;
    /* 默认速度：120 BPM = 500ms/四分音符 */
    uint32_t us_per_quarter = 500000;

    s_mid_note_count = 0;

    /* 解析每个轨道 */
    int offset = 14;
    for (int t = 0; t < num_tracks && offset + 8 <= size; t++) {
        /* 检查 MTrk */
        if (data[offset] != 'M' || data[offset+1] != 'T' ||
            data[offset+2] != 'r' || data[offset+3] != 'k') {
            ESP_LOGW(TAG, "Track %d: bad header", t);
            break;
        }
        int track_len = (data[offset+4] << 24) | (data[offset+5] << 16) |
                        (data[offset+6] << 8) | data[offset+7];
        int track_end = offset + 8 + track_len;
        if (track_end > size) track_end = size;

        int pos = offset + 8;
        uint32_t abs_tick = 0;        /* 绝对tick位置 */
        int last_status = 0;          /* 运行状态 */
        uint32_t last_note_on_tick[128]; /* 记录note on的tick，用于计算时长 */
        bool note_on_active[128] = {false};

        while (pos < track_end && s_mid_note_count < MID_MAX_NOTES) {
            /* delta-time */
            uint32_t delta = mid_read_varlen(data, track_end, &pos);
            abs_tick += delta;
            if (pos >= track_end) break;

            uint8_t ev = data[pos];
            /* 处理运行状态：如果最高位不是1，使用上一个状态 */
            if (ev & 0x80) {
                last_status = ev;
                pos++;
            } else {
                ev = last_status;
            }

            /* Meta事件 */
            if (ev == 0xFF) {
                if (pos >= track_end) break;
                uint8_t meta_type = data[pos++];
                uint32_t meta_len = mid_read_varlen(data, track_end, &pos);
                if (pos + (int)meta_len > track_end) break;
                if (meta_type == 0x51 && meta_len >= 3) {
                    /* 设置速度：3字节微秒/四分音符 */
                    us_per_quarter = (data[pos] << 16) | (data[pos+1] << 8) | data[pos+2];
                }
                pos += meta_len;
                continue;
            }

            /* SysEx事件 */
            if (ev == 0xF0 || ev == 0xF7) {
                uint32_t sysex_len = mid_read_varlen(data, track_end, &pos);
                pos += sysex_len;
                continue;
            }

            /* 通道消息 */
            int status = ev & 0xF0;

            if (status == 0x90) {
                /* Note On */
                if (pos + 2 > track_end) break;
                int note = data[pos];
                int velocity = data[pos+1];
                pos += 2;
                if (velocity == 0) {
                    /* velocity=0 表示note off */
                    if (note_on_active[note]) {
                        uint32_t dur_ticks = abs_tick - last_note_on_tick[note];
                        if (s_mid_note_count < MID_MAX_NOTES) {
                            s_mid_notes[s_mid_note_count].note = note;
                            s_mid_notes[s_mid_note_count].duration =
                                (uint32_t)((uint64_t)dur_ticks * us_per_quarter / ticks_per_quarter / 1000);
                            s_mid_note_count++;
                        }
                        note_on_active[note] = false;
                    }
                } else {
                    last_note_on_tick[note] = abs_tick;
                    note_on_active[note] = true;
                }
            } else if (status == 0x80) {
                /* Note Off */
                if (pos + 2 > track_end) break;
                int note = data[pos];
                pos += 2;
                if (note_on_active[note]) {
                    uint32_t dur_ticks = abs_tick - last_note_on_tick[note];
                    if (s_mid_note_count < MID_MAX_NOTES) {
                        s_mid_notes[s_mid_note_count].note = note;
                        s_mid_notes[s_mid_note_count].duration =
                            (uint32_t)((uint64_t)dur_ticks * us_per_quarter / ticks_per_quarter / 1000);
                        s_mid_note_count++;
                    }
                    note_on_active[note] = false;
                }
            } else if (status == 0xB0 || status == 0xC0 || status == 0xD0) {
                /* 控制/程序/通道压力：跳过 */
                int len = (status == 0xC0 || status == 0xD0) ? 1 : 2;
                pos += len;
            } else if (status == 0xE0) {
                /* 弯音：2字节 */
                pos += 2;
            } else if (status == 0xA0) {
                /* 触后：2字节 */
                pos += 2;
            } else {
                /* 未知事件，跳过1字节 */
                pos++;
            }
        }
        offset = track_end;
    }

    ESP_LOGI(TAG, "Parsed %d notes", s_mid_note_count);
    return s_mid_note_count;
}

/* ========== 播放控制 ========== */

static void midplayer_play(void)
{
    if (s_mid_note_count == 0) return;
    s_mid_play_pos = 0;
    s_mid_state = MID_STATE_PLAYING;
}

static void midplayer_pause(void)
{
    if (s_mid_state == MID_STATE_PLAYING) {
        s_mid_state = MID_STATE_PAUSED;
        drv_buzzer_stop();
    } else if (s_mid_state == MID_STATE_PAUSED) {
        s_mid_state = MID_STATE_PLAYING;
    }
}

static void midplayer_stop(void)
{
    s_mid_state = MID_STATE_IDLE;
    s_mid_play_pos = 0;
    drv_buzzer_stop();
}

/* ========== UI 刷新 ========== */

static void midplayer_refresh(void)
{
    if (!s_mid_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_mid_obj);
    s_mid_bar = NULL;

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    char buf[96];

    /* 文件名 */
    snprintf(buf, sizeof(buf), "> %s", s_mid_file_name);
    lv_obj_t *name_lbl = lv_label_create(s_mid_obj);
    lv_label_set_text(name_lbl, buf);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(name_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_pos(name_lbl, 4, 2);

    /* 音符数 */
    snprintf(buf, sizeof(buf), "音符: %d", s_mid_note_count);
    lv_obj_t *info_lbl = lv_label_create(s_mid_obj);
    lv_label_set_text(info_lbl, buf);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(info_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_pos(info_lbl, 4, font_px + 4);

    /* 播放状态 */
    const char *state_str = "空闲";
    if (s_mid_state == MID_STATE_PLAYING) state_str = "播放中";
    else if (s_mid_state == MID_STATE_PAUSED) state_str = "已暂停";
    else if (s_mid_state == MID_STATE_DONE) state_str = "播放完成";

    snprintf(buf, sizeof(buf), "状态: %s", state_str);
    lv_obj_t *state_lbl = lv_label_create(s_mid_obj);
    lv_label_set_text(state_lbl, buf);
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(state_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_pos(state_lbl, 4, font_px * 2 + 6);

    /* 进度条 + 进度文本 */
    if (s_mid_note_count > 0) {
        int pct = (s_mid_play_pos * 100) / s_mid_note_count;
        if (pct > 100) pct = 100;
        snprintf(buf, sizeof(buf), "进度: %d%% (%d/%d)", pct, s_mid_play_pos, s_mid_note_count);
        lv_obj_t *prog_lbl = lv_label_create(s_mid_obj);
        lv_label_set_text(prog_lbl, buf);
        lv_obj_set_style_text_color(prog_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(prog_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_pos(prog_lbl, 4, font_px * 3 + 8);

        /* LVGL 进度条组件 */
        lv_obj_t *bar = lv_bar_create(s_mid_obj);
        lv_obj_remove_style_all(bar);
        /* 背景 */
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        /* 指示器 */
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_size(bar, LCD_H_RES - 8, 10);
        lv_obj_set_pos(bar, 4, font_px * 4 + 10);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        s_mid_bar = bar;
    }

    /* 操作提示 */
    snprintf(buf, sizeof(buf), "A:播放/暂停  B:返回");
    lv_obj_t *hint_lbl = lv_label_create(s_mid_obj);
    lv_label_set_text(hint_lbl, buf);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_pos(hint_lbl, 4, LCD_V_RES - DOCK_H - font_px - 4);
}

/* ========== 播放任务 ========== */

static void midplayer_play_task(void *arg)
{
    while (1) {
        if (s_mid_state == MID_STATE_PLAYING && s_mid_note_count > 0) {
            if (s_mid_play_pos >= s_mid_note_count) {
                s_mid_state = MID_STATE_DONE;
                drv_buzzer_stop();
                midplayer_refresh();
                continue;
            }
            mid_note_t *n = &s_mid_notes[s_mid_play_pos];
            uint32_t dur = n->duration;
            if (dur < 20) dur = 20;
            if (dur > 2000) dur = 2000;
            drv_buzzer_play_note(n->note, dur);
            /* 音符间小间隔 */
            vTaskDelay(pdMS_TO_TICKS(5));
            s_mid_play_pos++;
            if (s_mid_play_pos % 16 == 0) {
                midplayer_refresh();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ========== 页面生命周期 ========== */

static void midplayer_init(void *data)
{
    ESP_LOGI(TAG, "MID player init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("MID播放");

    /* 读取 stash 获取文件路径 */
    page_stash_t *stash = ui_stash_pop();
    if (stash && stash->size > 0) {
        strncpy(s_mid_file_path, (const char*)stash->data, sizeof(s_mid_file_path) - 1);
        s_mid_file_path[sizeof(s_mid_file_path) - 1] = '\0';
        /* 提取文件名 */
        const char *base = strrchr(s_mid_file_path, '/');
        strncpy(s_mid_file_name, base ? base + 1 : s_mid_file_path, sizeof(s_mid_file_name) - 1);
        s_mid_file_name[sizeof(s_mid_file_name) - 1] = '\0';
    } else {
        strcpy(s_mid_file_name, "未指定文件");
    }

    /* 读取并解析MIDI文件 */
    s_mid_note_count = 0;
    s_mid_play_pos = 0;
    s_mid_state = MID_STATE_IDLE;

    if (s_mid_file_path[0]) {
        FILE *fp = fopen(s_mid_file_path, "rb");
        if (fp) {
            uint8_t *buf = (uint8_t*)malloc(MID_MAX_FILE);
            if (buf) {
                int rd = fread(buf, 1, MID_MAX_FILE, fp);
                if (rd > 0) {
                    mid_parse(buf, rd);
                }
                free(buf);
            }
            fclose(fp);
        } else {
            ESP_LOGE(TAG, "Cannot open MIDI file: %s", s_mid_file_path);
        }
    }

    /* 创建内容区 */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, ui_content_y());
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    s_mid_obj = content;

    ui_dock_create(scr, 1, 0);
    midplayer_refresh();

    /* 启动播放任务 */
    static TaskHandle_t s_play_task = NULL;
    if (!s_play_task) {
        xTaskCreate(midplayer_play_task, "mid_play", 4096, NULL, 5, &s_play_task);
    }
}

static void midplayer_destroy(void)
{
    ESP_LOGI(TAG, "MID player destroy");
    drv_buzzer_stop();
    s_mid_obj = NULL;
    s_mid_bar = NULL;
    s_mid_state = MID_STATE_IDLE;
    s_mid_play_pos = 0;
    s_mid_note_count = 0;
    s_mid_file_path[0] = '\0';
    s_mid_file_name[0] = '\0';
}

static bool midplayer_on_key(int key)
{
    if (key == KEY_B) {
        midplayer_stop();
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    if (key == KEY_A) {
        if (s_mid_note_count == 0) return true;
        if (s_mid_state == MID_STATE_PLAYING) {
            midplayer_pause();
        } else if (s_mid_state == MID_STATE_PAUSED) {
            midplayer_pause();  /* 恢复播放 */
        } else if (s_mid_state == MID_STATE_DONE) {
            midplayer_play();
        } else {
            midplayer_play();
        }
        midplayer_refresh();
        return true;
    }
    if (key == KEY_UP) {
        /* 音量增大（暂不支持，蜂鸣器固定音量） */
        return true;
    }
    if (key == KEY_DOWN) {
        /* 音量减小 */
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_midplayer_callbacks = {
    .init = midplayer_init,
    .destroy = midplayer_destroy,
    .on_key = midplayer_on_key,
};