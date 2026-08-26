/**
 * @file app_settings_datetime.c
 * @brief 日期时间设置二级页面 - 显示当前时间，支持NTP同步
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_datetime_settings_callbacks。
 * 显示当前日期时间，提供NTP同步功能（通过SNTP协议从网络获取时间）。
 * 需要WiFi已连接才能进行NTP同步。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "APP_DATETIME";

/* ========== NTP同步状态 ========== */
typedef enum {
    NTP_IDLE = 0,       // 空闲
    NTP_SYNCING,        // 同步中
    NTP_SUCCESS,        // 同步成功
    NTP_FAILED,         // 同步失败
} ntp_state_t;

static ntp_state_t s_ntp_state = NTP_IDLE;
static bool s_ntp_initialized = false;
static int64_t s_ntp_sync_start_time = 0;  /* NTP同步开始时间戳（微秒） */
#define NTP_TIMEOUT_MS    10000  /* NTP同步超时（10秒） */
/* s_ntp_status removed - use lang_get() directly in dt_refresh_label */

/* ========== NTP同步回调 ========== */
static void ntp_sync_callback(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP time synchronized successfully");
    s_ntp_state = NTP_SUCCESS;
}

/* ========== NTP同步函数 ========== */
static void ntp_sync_start(void)
{
    if (s_ntp_state == NTP_SYNCING) {
        ESP_LOGW(TAG, "NTP sync already in progress");
        return;
    }

    s_ntp_state = NTP_SYNCING;
    s_ntp_sync_start_time = esp_timer_get_time();  /* 记录开始时间 */

    // 设置时区为北京时间 (UTC+8)
    setenv("TZ", "CST-8", 1);
    tzset();

    if (!s_ntp_initialized) {
        ESP_LOGI(TAG, "Initializing SNTP");
        // 配置SNTP：使用国内NTP服务器池
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("cn.pool.ntp.org");
        config.sync_cb = ntp_sync_callback;
        config.renew_servers_after_new_IP = true;
        
        esp_netif_sntp_init(&config);
        
        // 添加备用服务器
        esp_sntp_setservername(1, "ntp.aliyun.com");
        esp_sntp_setservername(2, "time.nist.gov");
        
        s_ntp_initialized = true;
    } else {
        // 重新触发同步
        esp_netif_sntp_start();
    }

    ESP_LOGI(TAG, "SNTP started, waiting for time sync...");
}

/* ========== UI状态 ========== */
static lv_obj_t *s_dt_list = NULL;
static lv_obj_t *s_dt_labels[5] = {0};
static int s_dt_sel = 0;
static int s_dt_vis_rows = 6;
static int s_dt_row_h = 14;
static int s_dt_total = 5;

/* ========== 定时器自动刷新 ========== */
static lv_timer_t *s_dt_timer = NULL;

static void dt_refresh_label(int idx);  /* 前向声明 */

static void dt_timer_cb(lv_timer_t *t)
{
    /* 每秒刷新时间显示 */
    if (s_dt_list) {
        dt_refresh_label(0);  /* 日期 */
        dt_refresh_label(1);  /* 时间 */
        dt_refresh_label(3);  /* NTP状态详情 */

        /* NTP 超时检测：如果同步中且超过10秒未成功，标记为失败 */
        if (s_ntp_state == NTP_SYNCING) {
            int64_t elapsed = (esp_timer_get_time() - s_ntp_sync_start_time) / 1000;
            if (elapsed > NTP_TIMEOUT_MS) {
                ESP_LOGW(TAG, "NTP sync timed out (%lld ms)", elapsed);
                s_ntp_state = NTP_FAILED;
                dt_refresh_label(2);  /* 刷新NTP状态行 */
                dt_refresh_label(3);  /* 刷新详情行 */
            }
        }
    }
}

static void dt_refresh_label(int idx)
{
    if (!s_dt_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[48];

    time_t now;
    struct tm tm_info;
    time(&now);
    localtime_r(&now, &tm_info);

    switch (idx) {
    case 0:
        snprintf(buf, sizeof(buf), "%s: %04d-%02d-%02d",
                 lang_get(STR_DATE_TIME), tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
        break;
    case 1:
        snprintf(buf, sizeof(buf), "%s: %02d:%02d:%02d",
                 lang_get(STR_DATE_TIME), tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        break;
    case 2:
        // 显示NTP同步状态
        if (s_ntp_state == NTP_SYNCING) {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNC));
        } else if (s_ntp_state == NTP_SUCCESS) {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNC_OK));
        } else if (s_ntp_state == NTP_FAILED) {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNC_FAIL));
        } else {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNC_HINT));
        }
        break;
    case 3:
        // 显示NTP状态详情
        if (s_ntp_state == NTP_SUCCESS) {
            snprintf(buf, sizeof(buf), "%s: %02d:%02d:%02d",
                     lang_get(STR_CURRENT_VALUE), tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        } else if (s_ntp_state == NTP_SYNCING) {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNCING));
        } else {
            snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATETIME_SYNC_HINT));
        }
        break;
    case 4:
        snprintf(buf, sizeof(buf), "%s", lang_get(STR_BACK));
        break;
    default:
        buf[0] = '\0';
        break;
    }
    lv_label_set_text(s_dt_labels[idx], buf);
}

static void dt_rebuild_visible(void)
{
    if (!s_dt_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_dt_list);
    memset(s_dt_labels, 0, sizeof(s_dt_labels));
    for (int i = 0; i < s_dt_vis_rows && i < s_dt_total; i++) {
        lv_obj_t *row = lv_obj_create(s_dt_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_dt_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_dt_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_dt_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_dt_labels[i] = lbl;
        dt_refresh_label(i);
    }
}

static void dt_settings_init(void *data)
{
    ESP_LOGI(TAG, "Datetime settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_DATE_TIME));
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_dt_row_h = font_px + 2;
    s_dt_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_dt_row_h;
    if (s_dt_vis_rows < 1) s_dt_vis_rows = 1;
    s_dt_list = lv_obj_create(scr);
    if (!s_dt_list) {
        ESP_LOGE(TAG, "lv_obj_create(s_dt_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_dt_list);
    lv_obj_set_pos(s_dt_list, 0, ui_content_y());
    lv_obj_set_size(s_dt_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_dt_list, LV_OBJ_FLAG_SCROLLABLE);
    s_dt_sel = 0;
    
    dt_rebuild_visible();
    ui_dock_create(scr, 1, 0);
    
    /* 创建定时器，每秒刷新时间显示 */
    if (s_dt_timer) lv_timer_del(s_dt_timer);
    s_dt_timer = lv_timer_create(dt_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(s_dt_timer, -1);  /* 无限重复 */
}

static void dt_settings_destroy(void)
{
    ESP_LOGI(TAG, "Datetime settings destroy");
    if (s_dt_timer) {
        lv_timer_del(s_dt_timer);
        s_dt_timer = NULL;
    }
    s_dt_list = NULL;
    memset(s_dt_labels, 0, sizeof(s_dt_labels));
}

static bool dt_settings_on_key(int key)
{
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    
    if (key == KEY_UP || key == KEY_DOWN) {
        /* 刷新显示时间 */
        dt_rebuild_visible();
        return true;
    }
    
    if (key == KEY_A) {
        /* 按A键触发NTP同步 */
        ESP_LOGI(TAG, "NTP sync triggered by user");
        
        // 重置状态并开始同步
        s_ntp_state = NTP_IDLE;
        ntp_sync_start();
        
        dt_rebuild_visible();
        return true;
    }
    
    return false;
}

const page_callbacks_t g_datetime_settings_callbacks = {
    .init = dt_settings_init,
    .destroy = dt_settings_destroy,
    .on_key = dt_settings_on_key,
};