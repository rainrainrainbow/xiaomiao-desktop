/**
 * @file app_settings_sleep.c
 * @brief 屏幕超时设置二级页面 - 休眠时间选择 + 定时关机设置
 *
 * 定时关机：可配置 关闭/15分/30分/60分，配置与到期时间均存 NVS。
 * ESP32 无真正软断电能力，因此到期只在本页/状态栏提示"定时关机时间到"，
 * 不执行断电或重启（需硬件支持），避免在后台产生不可逆动作。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "APP_SLEEP";

#define SLEEP_OPTION_COUNT 5
static const int s_sleep_values[SLEEP_OPTION_COUNT] = {0, 30, 60, 120, 300};
/* 国际化：使用 lang_get 获取睡眠时间名称 */
static const char *sleep_name(int idx) {
    switch (idx) {
        case 0: return lang_get(STR_SLEEP_NEVER);
        case 1: return lang_get(STR_SLEEP_30S);
        case 2: return lang_get(STR_SLEEP_60S);
        case 3: return lang_get(STR_SLEEP_2M);
        case 4: return lang_get(STR_SLEEP_5M);
        default: return "";
    }
}

/* ========== 定时关机 ========== */
/* 不改动 app_cfg_key_t 枚举，用独立 NVS key 持久化本页配置 */
#define TIMER_NVS_KEY_MIN   "timer_off"      /* 定时分钟数 0/15/30/60 */
#define TIMER_NVS_KEY_AT    "timer_off_at"   /* 到期时刻(epoch秒, 0=未启用) */
#define TIMER_OPTION_COUNT  4
static const int s_timer_values[TIMER_OPTION_COUNT] = {0, 15, 30, 60};

static int s_timer_off_min = 0;       /* 当前配置的分钟数 */
static time_t s_timer_deadline = 0;   /* 到期 epoch 秒，0=未启用 */
static bool s_timer_fired = false;    /* 到期提示是否已触发 */

/* 这些档位没有独立 lang 字符串，沿用项目既有的字面量写法 */
static const char *timer_name(int idx) {
    switch (idx) {
        case 0: return "关闭";
        case 1: return "15分";
        case 2: return "30分";
        case 3: return "60分";
        default: return "";
    }
}

static int timer_current_idx(void)
{
    for (int i = 0; i < TIMER_OPTION_COUNT; i++) {
        if (s_timer_values[i] == s_timer_off_min) return i;
    }
    return 0;
}

/* 载入：分钟数 + 到期时刻（过期则丢弃） */
static void sleep_timer_restore(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    int64_t at = 0;
    s_timer_off_min = 0;
    s_timer_deadline = 0;
    s_timer_fired = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, TIMER_NVS_KEY_MIN, &v);
        nvs_get_i64(h, TIMER_NVS_KEY_AT, &at);
        nvs_close(h);
    }
    if (v >= 0 && v <= 60) s_timer_off_min = (int)v;
    /* 用 epoch 秒而非 esp_timer（重启会清零），重启后仍可正确判断到期 */
    if (at > (int64_t)time(NULL)) s_timer_deadline = (time_t)at;
}

static void sleep_timer_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, TIMER_NVS_KEY_MIN, s_timer_off_min);
        nvs_set_i64(h, TIMER_NVS_KEY_AT, (int64_t)s_timer_deadline);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 应用定时档位：写入配置并计算到期时刻 */
static void sleep_timer_apply(int idx)
{
    if (idx < 0 || idx >= TIMER_OPTION_COUNT) return;
    s_timer_off_min = s_timer_values[idx];
    s_timer_fired = false;
    if (s_timer_off_min > 0) {
        s_timer_deadline = time(NULL) + (time_t)s_timer_off_min * 60;
    } else {
        s_timer_deadline = 0;
    }
    sleep_timer_persist();
    ESP_LOGI(TAG, "Timer-off set to %d min (deadline=%lld)",
             s_timer_off_min, (long long)s_timer_deadline);
}

/* ========== 行索引 ==========
 * 0..4 屏幕超时选项 / 5 当前值 / 6 定时关机标题 / 7..10 定时档位 */
#define SLEEP_CUR_ROW       SLEEP_OPTION_COUNT
#define TIMER_HEAD_ROW      (SLEEP_OPTION_COUNT + 1)
#define TIMER_OPT_BASE      (SLEEP_OPTION_COUNT + 2)
#define SLEEP_TOTAL         (TIMER_OPT_BASE + TIMER_OPTION_COUNT)

static lv_obj_t *s_sleep_list = NULL;
static lv_obj_t *s_sleep_labels[SLEEP_TOTAL] = {0};
static lv_obj_t *s_sleep_checkboxes[SLEEP_OPTION_COUNT] = {0}; /* 复选框组件 */
static int s_sleep_sel = 0;
static int s_sleep_scroll = 0;
static int s_sleep_vis_rows = 6;
static int s_sleep_row_h = 14;
static int s_sleep_total = SLEEP_TOTAL;
static lv_timer_t *s_sleep_timer = NULL;  /* 到期检测定时器 */

/* 刷新复选框状态 */
static void sleep_refresh_checkboxes(void)
{
    ui_state_t *st = ui_state_get();
    for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
        if (s_sleep_checkboxes[i]) {
            if (s_sleep_values[i] == st->sleep_timeout) {
                lv_obj_add_state(s_sleep_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_sleep_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void sleep_refresh_label(int idx)
{
    if (!s_sleep_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[56];
    if (idx < SLEEP_OPTION_COUNT) {
        snprintf(buf, sizeof(buf), "%s", sleep_name(idx));
    } else if (idx == SLEEP_CUR_ROW) {
        const char *cur = sleep_name(0);
        for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
            if (s_sleep_values[i] == st->sleep_timeout) { cur = sleep_name(i); break; }
        }
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_CURRENT_VALUE), cur);
    } else if (idx == TIMER_HEAD_ROW) {
        /* 到期只提示，不真正断电（ESP32 无软断电，需硬件支持） */
        if (s_timer_fired) {
            snprintf(buf, sizeof(buf), "定时关机时间到");
        } else if (s_timer_deadline > 0) {
            int64_t remain = (int64_t)s_timer_deadline - (int64_t)time(NULL);
            if (remain < 0) remain = 0;
            snprintf(buf, sizeof(buf), "%s: %s (%dm)", lang_get(STR_SET_TIMER_OFF),
                     timer_name(timer_current_idx()), (int)((remain + 59) / 60));
        } else {
            snprintf(buf, sizeof(buf), "%s: %s (需硬件支持)", lang_get(STR_SET_TIMER_OFF),
                     timer_name(timer_current_idx()));
        }
    } else if (idx >= TIMER_OPT_BASE && idx < SLEEP_TOTAL) {
        int t = idx - TIMER_OPT_BASE;
        const char *mark = (s_timer_values[t] == s_timer_off_min) ? "* " : "  ";
        snprintf(buf, sizeof(buf), "%s%s", mark, timer_name(t));
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(s_sleep_labels[idx], buf);
}

/* 到期检测：每秒检查一次，触发后清掉 NVS 到期时刻，避免重启后重复提示 */
static void sleep_timer_cb(lv_timer_t *t)
{
    if (s_timer_deadline == 0 || time(NULL) < s_timer_deadline) return;
    s_timer_deadline = 0;
    s_timer_fired = true;
    sleep_timer_persist();
    ui_statusbar_set_title("定时关机时间到");
    sleep_refresh_label(TIMER_HEAD_ROW);
    ESP_LOGI(TAG, "Timer-off reached: prompt only, no power cut (hardware unsupported)");
}

static void sleep_rebuild_visible(void)
{
    if (!s_sleep_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_sleep_list);
    memset(s_sleep_labels, 0, sizeof(s_sleep_labels));
    memset(s_sleep_checkboxes, 0, sizeof(s_sleep_checkboxes));
    s_sleep_total = SLEEP_TOTAL;
    if (s_sleep_sel < s_sleep_scroll) s_sleep_scroll = s_sleep_sel;
    if (s_sleep_sel >= s_sleep_scroll + s_sleep_vis_rows) s_sleep_scroll = s_sleep_sel - s_sleep_vis_rows + 1;
    if (s_sleep_scroll > s_sleep_total - s_sleep_vis_rows) s_sleep_scroll = s_sleep_total - s_sleep_vis_rows;
    if (s_sleep_scroll < 0) s_sleep_scroll = 0;
    for (int i = 0; i < s_sleep_vis_rows && (s_sleep_scroll + i) < s_sleep_total; i++) {
        int idx = s_sleep_scroll + i;
        lv_obj_t *row = lv_obj_create(s_sleep_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_sleep_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_sleep_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_sleep_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        if (idx < SLEEP_OPTION_COUNT) {
            /* 使用LVGL复选框组件 */
            lv_obj_t *cb = lv_checkbox_create(row);
            lv_checkbox_set_text(cb, sleep_name(idx));
            lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
            if (s_sleep_values[idx] == st->sleep_timeout) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
            }
            s_sleep_checkboxes[idx] = cb;
            s_sleep_labels[idx] = lv_label_create(row); /* 占位 */
        } else {
            lv_obj_t *lbl = lv_label_create(row);
            if (!lbl) {
                ESP_LOGE(TAG, "lv_label_create(lbl) failed! idx=%d mem free=%lu", idx,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
            lv_obj_set_width(lbl, LCD_H_RES - 12);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
            s_sleep_labels[idx] = lbl;
            sleep_refresh_label(idx);
        }
    }
}

static void sleep_settings_init(void *data)
{
    ESP_LOGI(TAG, "Sleep timeout settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_SLEEP_TIMEOUT));
    sleep_timer_restore();

    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_sleep_row_h = font_px + 2;
    s_sleep_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_sleep_row_h;
    if (s_sleep_vis_rows < 1) s_sleep_vis_rows = 1;
    s_sleep_list = lv_obj_create(scr);
    if (!s_sleep_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_sleep_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_sleep_list);
    lv_obj_set_pos(s_sleep_list, 0, ui_content_y());
    lv_obj_set_size(s_sleep_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_sleep_list, LV_OBJ_FLAG_SCROLLABLE);
    s_sleep_sel = 0; s_sleep_scroll = 0;
    for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
        if (s_sleep_values[i] == st->sleep_timeout) { s_sleep_sel = i; break; }
    }
    sleep_rebuild_visible();
    ui_dock_create(scr, 1, 0);

    /* 到期检测定时器（1秒），只在本页生命周期内运行 */
    if (s_sleep_timer) lv_timer_del(s_sleep_timer);
    s_sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_sleep_timer, -1);
}

static void sleep_settings_destroy(void)
{
    ESP_LOGI(TAG, "Sleep timeout settings destroy");
    if (s_sleep_timer) {
        lv_timer_del(s_sleep_timer);
        s_sleep_timer = NULL;
    }
    s_sleep_list = NULL;
    memset(s_sleep_labels, 0, sizeof(s_sleep_labels));
    memset(s_sleep_checkboxes, 0, sizeof(s_sleep_checkboxes));
}

static bool sleep_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_UP) { s_sleep_sel = (s_sleep_sel - 1 + s_sleep_total) % s_sleep_total; sleep_rebuild_visible(); return true; }
    if (key == KEY_DOWN) { s_sleep_sel = (s_sleep_sel + 1) % s_sleep_total; sleep_rebuild_visible(); return true; }
    if (key == KEY_A) {
        if (s_sleep_sel < SLEEP_OPTION_COUNT) {
            /* 屏幕超时 */
            int new_val = s_sleep_values[s_sleep_sel];
            if (new_val != st->sleep_timeout) { st->sleep_timeout = new_val; sleep_refresh_checkboxes(); sleep_rebuild_visible(); }
        } else if (s_sleep_sel >= TIMER_OPT_BASE && s_sleep_sel < TIMER_OPT_BASE + TIMER_OPTION_COUNT) {
            /* 定时关机：仅配置+到期提示，不真正断电 */
            sleep_timer_apply(s_sleep_sel - TIMER_OPT_BASE);
            sleep_rebuild_visible();
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_sleep_settings_callbacks = {
    .init = sleep_settings_init,
    .destroy = sleep_settings_destroy,
    .on_key = sleep_settings_on_key,
};
