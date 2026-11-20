/**
 * @file app_settings_bt.c
 * @brief 蓝牙设置页面 - 显示蓝牙状态、对端设备、配对操作
 *
 * 背景：此前蓝牙只有 A2DP 音频后端，没有独立 UI 入口，
 * 用户反馈"蓝牙没有界面"。本页提供：
 *  - 蓝牙初始化状态（开/关）
 *  - 连接状态（已连接/等待连接）与对端设备名
 *  - 本机蓝牙名称（供手机搜索）
 *  - A 键：重新进入可被发现模式，方便配对
 *
 * A2DP Sink 在 audio_output_init() 时已初始化并保持常驻，避免反复
 * deinit/init 蓝牙控制器带来的不稳定；本页面负责展示状态与配对触发。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "driver/drv_audio_bt_a2dp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_BT";

/* 行索引 */
#define BT_ROW_STATUS  0
#define BT_ROW_CONN    1
#define BT_ROW_PEER    2
#define BT_ROW_NAME    3
#define BT_ROW_PAIR    4
#define BT_ROW_COUNT   5

static lv_obj_t *s_bt_list = NULL;
static lv_obj_t *s_bt_labels[BT_ROW_COUNT] = {0};
static lv_obj_t *s_bt_rows[BT_ROW_COUNT] = {0};
static lv_timer_t *s_bt_timer = NULL;
static int s_bt_sel = 0;
static char s_bt_toast[64] = {0};       /* 一次性提示信息（配对结果） */
static uint32_t s_bt_toast_tick = 0;

static void bt_settings_refresh(void)
{
    if (!s_bt_list) return;
    ui_state_t *st = ui_state_get();
    char buf[80];

    bool inited = bt_a2dp_is_initialized();
    bool connected = bt_a2dp_is_connected();

    /* 状态行 */
    snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_BT_STATUS),
             inited ? lang_get(STR_BT_ON) : lang_get(STR_BT_OFF));
    if (s_bt_labels[BT_ROW_STATUS]) lv_label_set_text(s_bt_labels[BT_ROW_STATUS], buf);

    /* 连接状态行 */
    snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_BT_CONNECTED),
             connected ? lang_get(STR_BT_CONNECTED) : lang_get(STR_BT_IDLE));
    if (s_bt_labels[BT_ROW_CONN]) lv_label_set_text(s_bt_labels[BT_ROW_CONN], buf);

    /* 对端设备名 */
    char peer[32] = {0};
    if (connected) {
        bt_a2dp_get_peer_name(peer, sizeof(peer));
    } else {
        snprintf(peer, sizeof(peer), "-");
    }
    snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_BT_PEER), peer);
    if (s_bt_labels[BT_ROW_PEER]) lv_label_set_text(s_bt_labels[BT_ROW_PEER], buf);

    /* 本机蓝牙名（供手机搜索） */
    snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_WIFI_NAME), BT_DEVICE_NAME);
    if (s_bt_labels[BT_ROW_NAME]) lv_label_set_text(s_bt_labels[BT_ROW_NAME], buf);

    /* 配对操作行：显示 toast 或提示 */
    if (s_bt_toast[0] && (lv_tick_get() - s_bt_toast_tick) < 4000) {
        snprintf(buf, sizeof(buf), "%s", s_bt_toast);
    } else {
        s_bt_toast[0] = '\0';
        snprintf(buf, sizeof(buf), "%s", lang_get(STR_BT_PAIR_MODE));
    }
    if (s_bt_labels[BT_ROW_PAIR]) lv_label_set_text(s_bt_labels[BT_ROW_PAIR], buf);

    /* 高亮选中行（每次刷新重建样式，避免定时器与按键不同步） */
    const theme_colors_t *colors = ui_theme_colors();
    for (int i = 0; i < BT_ROW_COUNT; i++) {
        lv_obj_t *row = s_bt_rows[i];
        if (!row) continue;
        if (i == s_bt_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
    }
    (void)st;
}

static void bt_settings_rebuild(void)
{
    if (!s_bt_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 4;

    lv_obj_clean(s_bt_list);
    memset(s_bt_labels, 0, sizeof(s_bt_labels));
    memset(s_bt_rows, 0, sizeof(s_bt_rows));

    for (int i = 0; i < BT_ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_bt_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) continue;
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 12);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_bt_labels[i] = lbl;
        s_bt_rows[i] = row;
    }

    /* 底部提示行 */
    lv_obj_t *hint = lv_label_create(s_bt_list);
    if (hint) {
        lv_obj_set_style_text_color(hint, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_pos(hint, 0, BT_ROW_COUNT * row_h + 4);
        lv_label_set_text(hint, lang_get(STR_BT_HINT));
    }

    bt_settings_refresh();
}

static void bt_timer_cb(lv_timer_t *timer)
{
    bt_settings_refresh();
}

static void bt_settings_init(void *data)
{
    ESP_LOGI(TAG, "Bluetooth settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_BLUETOOTH));

    s_bt_list = lv_obj_create(scr);
    if (!s_bt_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_bt_list) failed! mem=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_bt_list);
    lv_obj_set_pos(s_bt_list, 0, ui_content_y());
    lv_obj_set_size(s_bt_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_bt_list, LV_OBJ_FLAG_SCROLLABLE);

    s_bt_sel = 0;
    s_bt_toast[0] = '\0';
    bt_settings_rebuild();
    ui_dock_create(scr, 1, 0);

    if (s_bt_timer == NULL) {
        s_bt_timer = lv_timer_create(bt_timer_cb, 500, NULL);
    }
}

static void bt_settings_destroy(void)
{
    ESP_LOGI(TAG, "Bluetooth settings destroy");
    if (s_bt_timer) {
        lv_timer_del(s_bt_timer);
        s_bt_timer = NULL;
    }
    s_bt_list = NULL;
    memset(s_bt_labels, 0, sizeof(s_bt_labels));
    memset(s_bt_rows, 0, sizeof(s_bt_rows));
}

static bool bt_settings_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    if (key == KEY_UP) {
        s_bt_sel = (s_bt_sel - 1 + BT_ROW_COUNT) % BT_ROW_COUNT;
        bt_settings_refresh();
        return true;
    }
    if (key == KEY_DOWN) {
        s_bt_sel = (s_bt_sel + 1) % BT_ROW_COUNT;
        bt_settings_refresh();
        return true;
    }
    if (key == KEY_A) {
        /* A 键：重新进入可被发现模式，方便手机搜索配对 */
        esp_err_t ret = bt_a2dp_start_discoverable();
        snprintf(s_bt_toast, sizeof(s_bt_toast), "%s",
                 ret == ESP_OK ? lang_get(STR_BT_PAIR_OK) : lang_get(STR_BT_PAIR_FAIL));
        s_bt_toast_tick = lv_tick_get();
        bt_settings_refresh();
        return true;
    }
    return false;
}

const page_callbacks_t g_bt_settings_callbacks = {
    .init = bt_settings_init,
    .destroy = bt_settings_destroy,
    .on_key = bt_settings_on_key,
};
