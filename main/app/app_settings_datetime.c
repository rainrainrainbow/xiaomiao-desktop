/**
 * @file app_settings_datetime.c
 * @brief 日期时间设置二级页面 - 显示当前时间，支持NTP同步 / 手动设置 / 12小时制 / 时区说明
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_datetime_settings_callbacks。
 * 显示当前日期时间，提供NTP同步功能（通过SNTP协议从网络获取时间）。
 * 需要WiFi已连接才能进行NTP同步。NTP 自动获取逻辑保持不变，本页只在其上补齐交互项。
 *
 * 说明：状态栏时间格式化在 ui_framework.c（本任务不可改），因此 12/24 小时制
 * 只在本页的"时间"行显示样例，并标注"部分支持"；时区固定 UTC+8，标注其它时区暂不支持。
 */
#include "esp_heap_caps.h"
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

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

/* ========== 新增设置项：12/24小时制 ========== */
/* 不改动 app_cfg_key_t 枚举，使用独立 NVS key 持久化本页偏好 */
#define DT_NVS_KEY_12H   "dt_12h"
static bool s_dt_12h = false;   /* false=24小时制(默认)，true=12小时制 */

static void dt_load_12h(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, DT_NVS_KEY_12H, &v);
        nvs_close(h);
    }
    s_dt_12h = (v != 0);
}

static void dt_save_12h(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, DT_NVS_KEY_12H, s_dt_12h ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ========== 新增设置项：手动设置时间（上下键调字段，A保存/B取消） ========== */
static bool s_dt_edit_mode = false;
static int  s_dt_edit_field = 0;             /* 0=年 1=月 2=日 3=时 4=分 */
static struct tm s_dt_edit_tm;

static const char *dt_field_name(int field)
{
    switch (field) {
        case 0: return "年";
        case 1: return "月";
        case 2: return "日";
        case 3: return "时";
        case 4: return "分";
        default: return "";
    }
}

static int dt_field_value(int field)
{
    switch (field) {
        case 0: return s_dt_edit_tm.tm_year + 1900;
        case 1: return s_dt_edit_tm.tm_mon + 1;
        case 2: return s_dt_edit_tm.tm_mday;
        case 3: return s_dt_edit_tm.tm_hour;
        case 4: return s_dt_edit_tm.tm_min;
        default: return 0;
    }
}

/* 字段加减：月/日/时/分采用循环，年限制在 2000~2100 */
static void dt_field_adjust(int field, int delta)
{
    int v = dt_field_value(field) + delta;
    switch (field) {
        case 0:
            if (v < 2000) v = 2000;
            if (v > 2100) v = 2100;
            s_dt_edit_tm.tm_year = v - 1900;
            break;
        case 1:
            if (v < 1) v = 12;
            if (v > 12) v = 1;
            s_dt_edit_tm.tm_mon = v - 1;
            break;
        case 2:
            if (v < 1) v = 31;
            if (v > 31) v = 1;
            s_dt_edit_tm.tm_mday = v;
            break;
        case 3:
            if (v < 0) v = 23;
            if (v > 23) v = 0;
            s_dt_edit_tm.tm_hour = v;
            break;
        case 4:
            if (v < 0) v = 59;
            if (v > 59) v = 0;
            s_dt_edit_tm.tm_min = v;
            break;
        default:
            break;
    }
}

/* ========== UI状态 ========== */
#define DT_ROW_DATE   0   /* 日期（显示） */
#define DT_ROW_TIME   1   /* 时间（显示，同时作为 12/24 小时制样例） */
#define DT_ROW_NTP    2   /* NTP同步状态（A键触发同步） */
#define DT_ROW_MANUAL 3   /* 手动设置时间（A键进入编辑） */
#define DT_ROW_12H    4   /* 12/24小时制（A键切换） */
#define DT_ROW_TZ     5   /* 时区说明（固定 UTC+8） */
#define DT_ROW_BACK   6   /* 返回 */
#define DT_TOTAL      7

static lv_obj_t *s_dt_list = NULL;
static lv_obj_t *s_dt_labels[DT_TOTAL] = {0};
static int s_dt_sel = 0;
static int s_dt_scroll = 0;
static int s_dt_vis_rows = 6;
static int s_dt_row_h = 14;

/* ========== 定时器自动刷新 ========== */
static lv_timer_t *s_dt_timer = NULL;

static void dt_refresh_label(int idx);  /* 前向声明 */

static void dt_timer_cb(lv_timer_t *t)
{
    if (!s_dt_list) return;

    /* NTP 超时检测：即使处于手动编辑态也继续跑，避免同步状态卡死 */
    if (s_ntp_state == NTP_SYNCING) {
        int64_t elapsed = (esp_timer_get_time() - s_ntp_sync_start_time) / 1000;
        if (elapsed > NTP_TIMEOUT_MS) {
            ESP_LOGW(TAG, "NTP sync timed out (%lld ms)", elapsed);
            s_ntp_state = NTP_FAILED;
            dt_refresh_label(DT_ROW_NTP);
        }
    }

    /* 编辑态下字段行由按键驱动刷新，定时器只负责普通态的实时时间 */
    if (!s_dt_edit_mode) {
        dt_refresh_label(DT_ROW_DATE);  /* 日期 */
        dt_refresh_label(DT_ROW_TIME);  /* 时间（12/24 样例） */
    }
}

static void dt_refresh_label(int idx)
{
    if (!s_dt_labels[idx]) return;
    char buf[56];

    /* 手动编辑态：前5行显示 年/月/日/时/分，第6行显示操作提示 */
    if (s_dt_edit_mode) {
        if (idx < 5) {
            snprintf(buf, sizeof(buf), "%s: %04d", dt_field_name(idx), dt_field_value(idx));
        } else if (idx == 5) {
            snprintf(buf, sizeof(buf), "A=保存 B=取消");
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(s_dt_labels[idx], buf);
        return;
    }

    time_t now;
    struct tm tm_info;
    time(&now);
    localtime_r(&now, &tm_info);

    switch (idx) {
    case DT_ROW_DATE:
        snprintf(buf, sizeof(buf), "%s: %04d-%02d-%02d",
                 lang_get(STR_DATE_TIME), tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
        break;
    case DT_ROW_TIME:
        /* 本页样例时间：按 12/24 偏好格式化（状态栏格式在 ui_framework.c，不可改，
         * 故在 DT_ROW_12H 行标注"部分支持"） */
        if (s_dt_12h) {
            int h12 = tm_info.tm_hour % 12;
            if (h12 == 0) h12 = 12;
            snprintf(buf, sizeof(buf), "%s: %02d:%02d:%02d %s", lang_get(STR_DATE_TIME),
                     h12, tm_info.tm_min, tm_info.tm_sec, tm_info.tm_hour < 12 ? "AM" : "PM");
        } else {
            snprintf(buf, sizeof(buf), "%s: %02d:%02d:%02d",
                     lang_get(STR_DATE_TIME), tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        }
        break;
    case DT_ROW_NTP:
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
    case DT_ROW_MANUAL:
        snprintf(buf, sizeof(buf), "%s  >", lang_get(STR_SET_TIME_MANUAL));
        break;
    case DT_ROW_12H:
        /* 样例在上一行；状态栏格式化不由本页控制，明确标注部分支持 */
        snprintf(buf, sizeof(buf), "%s: %s (部分支持)", lang_get(STR_SET_TIME_12H),
                 s_dt_12h ? "ON" : "OFF");
        break;
    case DT_ROW_TZ:
        snprintf(buf, sizeof(buf), "%s: UTC+8 (其它暂不支持)", lang_get(STR_SET_TIMEZONE));
        break;
    case DT_ROW_BACK:
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
    int sel = s_dt_edit_mode ? s_dt_edit_field : s_dt_sel;  /* 编辑态高亮当前字段 */

    lv_obj_clean(s_dt_list);
    memset(s_dt_labels, 0, sizeof(s_dt_labels));

    /* 确保选中项在可见范围内；编辑态用 sel=字段索引，大字体下也能滚到"分" */
    if (sel < s_dt_scroll) s_dt_scroll = sel;
    if (sel >= s_dt_scroll + s_dt_vis_rows) s_dt_scroll = sel - s_dt_vis_rows + 1;
    if (s_dt_scroll > DT_TOTAL - s_dt_vis_rows) s_dt_scroll = DT_TOTAL - s_dt_vis_rows;
    if (s_dt_scroll < 0) s_dt_scroll = 0;

    for (int i = 0; i < s_dt_vis_rows && (s_dt_scroll + i) < DT_TOTAL; i++) {
        int idx = s_dt_scroll + i;
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
        if (idx == sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
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
        s_dt_labels[idx] = lbl;
        dt_refresh_label(idx);
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

    /* 保证未走 NTP 同步时本地时间也是 UTC+8（与原 ntp_sync_start 保持一致） */
    setenv("TZ", "CST-8", 1);
    tzset();
    dt_load_12h();

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
    s_dt_scroll = 0;
    s_dt_edit_mode = false;

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
    s_dt_edit_mode = false;
    memset(s_dt_labels, 0, sizeof(s_dt_labels));
}

static bool dt_settings_on_key(int key)
{
    if (key == KEY_B) {
        if (s_dt_edit_mode) {
            /* 手动编辑态：B 取消，不写回系统时间 */
            s_dt_edit_mode = false;
            dt_rebuild_visible();
        } else if (ui_stack_depth() > 1) {
            ui_stack_pop();
        }
        return true;
    }

    /* ========== 手动设置时间编辑态 ========== */
    if (s_dt_edit_mode) {
        if (key == KEY_UP) {
            s_dt_edit_field = (s_dt_edit_field + 4) % 5;
            dt_rebuild_visible();
            return true;
        }
        if (key == KEY_DOWN) {
            s_dt_edit_field = (s_dt_edit_field + 1) % 5;
            dt_rebuild_visible();
            return true;
        }
        if (key == KEY_LEFT) {
            dt_field_adjust(s_dt_edit_field, -1);
            dt_rebuild_visible();
            return true;
        }
        if (key == KEY_RIGHT) {
            dt_field_adjust(s_dt_edit_field, 1);
            dt_rebuild_visible();
            return true;
        }
        if (key == KEY_A) {
            /* 用 mktime 按 TZ=CST-8 换算为 UTC 时间戳，再 settimeofday 写入系统时间 */
            struct timeval tv;
            s_dt_edit_tm.tm_sec = 0;
            s_dt_edit_tm.tm_isdst = -1;
            tv.tv_sec = mktime(&s_dt_edit_tm);
            tv.tv_usec = 0;
            if (tv.tv_sec != (time_t)-1 && settimeofday(&tv, NULL) == 0) {
                ESP_LOGI(TAG, "Manual time set: %04d-%02d-%02d %02d:%02d",
                         s_dt_edit_tm.tm_year + 1900, s_dt_edit_tm.tm_mon + 1,
                         s_dt_edit_tm.tm_mday, s_dt_edit_tm.tm_hour, s_dt_edit_tm.tm_min);
            } else {
                ESP_LOGE(TAG, "settimeofday failed");
            }
            s_dt_edit_mode = false;
            dt_rebuild_visible();
            return true;
        }
        return true;
    }

    /* ========== 普通列表态 ========== */
    if (key == KEY_UP) {
        s_dt_sel = (s_dt_sel - 1 + DT_TOTAL) % DT_TOTAL;
        dt_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_dt_sel = (s_dt_sel + 1) % DT_TOTAL;
        dt_rebuild_visible();
        return true;
    }
    if (key == KEY_A) {
        switch (s_dt_sel) {
        case DT_ROW_NTP: {
            ESP_LOGI(TAG, "NTP sync triggered by user");
            s_ntp_state = NTP_IDLE;
            ntp_sync_start();
            dt_rebuild_visible();
            break;
        }
        case DT_ROW_MANUAL: {
            /* 预填当前本地时间，进入上下键字段编辑 */
            time_t now;
            struct tm tm_info;
            time(&now);
            localtime_r(&now, &tm_info);
            s_dt_edit_tm = tm_info;
            s_dt_edit_tm.tm_sec = 0;
            s_dt_edit_field = 0;
            s_dt_edit_mode = true;
            dt_rebuild_visible();
            break;
        }
        case DT_ROW_12H:
            s_dt_12h = !s_dt_12h;
            dt_save_12h();
            dt_rebuild_visible();
            break;
        case DT_ROW_TZ:
            /* 固定 UTC+8，无时区选择 UI；提示已在标签中给出 */
            break;
        case DT_ROW_BACK:
            if (ui_stack_depth() > 1) ui_stack_pop();
            break;
        default:
            break;
        }
        return true;
    }
    return false;
}

const page_callbacks_t g_datetime_settings_callbacks = {
    .init = dt_settings_init,
    .destroy = dt_settings_destroy,
    .on_key = dt_settings_on_key,
};
