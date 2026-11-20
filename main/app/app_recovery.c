/**
 * @file app_recovery.c
 * @brief 恢复模式（Recovery）页面 - 类安卓 Recovery 的系统维护菜单
 *
 * 背景：原设计只有"返回Loader"动作，没有真正的恢复界面。本页提供：
 *  - 恢复出厂设置：清空 xiaomiao NVS 命名空间的所有用户设置
 *  - 清除 WiFi 凭据：删除 NVS 中保存的 SSID / 密码
 *  - 重新开始 OTA 升级：进入 OTA 页面（ota_start 为 app_ota.c 内部 static，
 *    不便跨文件调用；进入该页后按 A 即重新触发升级，避免改动他人文件）
 *  - 从 SD 卡刷机：检测 /sdcard 常见固件名并给出明确提示。刻意不直接
 *    覆写系统分区，避免误操作变砖；真正刷机仍走 OTA 流程
 *  - 查看系统信息：固件版本、分区表数量、可用内存（DRAM/PSRAM）
 *  - 重启到主系统：设置 launcher(ota_0) 为启动分区并重启
 *  - 重启到 Loader(factory)：复用 return_to_loader_setup() + esp_restart()
 *
 * 关键约束（用户明确要求，务必遵守）：
 *  - 取消"崩溃/重启后自动回退 factory"的行为。本页面绝不在 init、定时器、
 *    崩溃回调或任何自动路径里调用 return_to_loader_setup() /
 *    esp_ota_set_boot_partition(factory)。
 *  - return_to_loader_setup() 只在「重启到 Loader」菜单项经用户二次确认后
 *    显式调用一次，是全文件唯一允许设置 factory 为下次启动分区的位置。
 *  - 主系统(launcher/ota_0)启动后不设置下次启动分区，保持 ota_0，
 *    这样 OTA 升级后重启才会进入新固件；「重启到主系统」只设置 ota_0，
 *    找不到 ota_0 时提示失败，绝不回退到 factory。
 *
 * 交互约定：
 *  - 上下键选择，A 键二次确认（再按一次 A 才执行），B 键取消/返回
 *  - 破坏性操作（清空设置/清凭据/刷机/重启）必须二次确认，避免误触
 *  - 160x128 小屏：固定 5 行可视 + 1 行状态，字体上限 16px，布局紧凑
 *  - LVGL 定时器回调只改文本/样式，绝不 create/delete 对象（防 glyph cache UAF）
 *  - 所有 LVGL 对象创建后判空，内存不足时记录日志并优雅降级
 */
#include "app_builtin.h"
#include "app_recovery.h"
#include "app_ota.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "system/sys_nvs.h"
#include "system/sys_sdcard.h"
#include "return_to_loader.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>

static const char *TAG = "APP_RECOVERY";

/* ========== 菜单项定义 ========== */
#define REC_ITEM_FACTORY_RESET 0
#define REC_ITEM_CLEAR_WIFI    1
#define REC_ITEM_OTA_RESTART   2
#define REC_ITEM_SD_FLASH      3
#define REC_ITEM_SYS_INFO      4
#define REC_ITEM_REBOOT_MAIN   5
#define REC_ITEM_REBOOT_LOADER 6
#define REC_ITEM_COUNT         7

#define REC_VIS_ROWS  5   /* 小屏可视行数 */

/* ========== 运行状态 ========== */
static lv_obj_t *s_rec_list = NULL;
static lv_obj_t *s_rec_rows[REC_VIS_ROWS] = {0};
static lv_obj_t *s_rec_labels[REC_VIS_ROWS] = {0};
static lv_obj_t *s_rec_status_obj = NULL;
static lv_timer_t *s_rec_timer = NULL;
static int s_rec_row_h = 16;
static int s_rec_sel = 0;
static int s_rec_scroll = 0;
static int s_rec_confirm = -1;              /* 待二次确认的菜单项，-1 表示无 */
static int s_rec_view = 0;                  /* 0=菜单, 1=系统信息 */
static char s_rec_status[96] = {0};         /* 一次性状态提示 */
static uint32_t s_rec_status_tick = 0;
static char s_rec_info[REC_VIS_ROWS][96] = {{0}};  /* 系统信息视图缓存文本 */

/* ========== 工具函数 ========== */
static void rec_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_rec_status, sizeof(s_rec_status), fmt, ap);
    va_end(ap);
    s_rec_status_tick = lv_tick_get();
}

static const char *rec_item_label(int idx)
{
    switch (idx) {
    case REC_ITEM_FACTORY_RESET: return lang_get(STR_REC_FACTORY_RESET);
    case REC_ITEM_CLEAR_WIFI:    return lang_get(STR_REC_CLEAR_WIFI);
    case REC_ITEM_OTA_RESTART:   return lang_get(STR_REC_OTA_RESTART);
    case REC_ITEM_SD_FLASH:      return lang_get(STR_REC_SD_FLASH);
    case REC_ITEM_SYS_INFO:      return lang_get(STR_REC_SYS_INFO);
    case REC_ITEM_REBOOT_MAIN:   return lang_get(STR_REC_REBOOT_MAIN);
    case REC_ITEM_REBOOT_LOADER: return lang_get(STR_RETURN_LOADER);
    default: return "";
    }
}

static void rec_scroll_to_sel(void)
{
    if (s_rec_sel < s_rec_scroll) {
        s_rec_scroll = s_rec_sel;
    } else if (s_rec_sel >= s_rec_scroll + REC_VIS_ROWS) {
        s_rec_scroll = s_rec_sel - REC_VIS_ROWS + 1;
    }
    if (s_rec_scroll > REC_ITEM_COUNT - REC_VIS_ROWS) {
        s_rec_scroll = REC_ITEM_COUNT - REC_VIS_ROWS;
    }
    if (s_rec_scroll < 0) s_rec_scroll = 0;
}

/* ========== 系统信息采集（进入信息视图时调用一次，避免定时器频繁遍历） ========== */
static void rec_collect_info(void)
{
    /* 启动分区 */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const char *boot_name = "-";
    if (boot && boot->label[0]) boot_name = boot->label;

    /* 分区表数量：注意 esp_partition_next() 到达末尾返回 NULL 时已自动释放迭代器 */
    int parts = 0;
    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        parts++;
        it = esp_partition_next(it);
    }

    /* 可用内存 */
    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;

    snprintf(s_rec_info[0], sizeof(s_rec_info[0]), "%s: %s",
             lang_get(STR_VERSION), XIAOMIAO_VERSION);
    snprintf(s_rec_info[1], sizeof(s_rec_info[1]), "%s: %s",
             lang_get(STR_BUILD), XIAOMIAO_BUILD);
    snprintf(s_rec_info[2], sizeof(s_rec_info[2]), "%s: %s",
             lang_get(STR_REC_INFO_BOOT), boot_name);
    snprintf(s_rec_info[3], sizeof(s_rec_info[3]), "%s: %d",
             lang_get(STR_REC_INFO_PARTS), parts);
    if (psram_total > 0) {
        snprintf(s_rec_info[4], sizeof(s_rec_info[4]), "%s: %luK/%luK",
                 lang_get(STR_REC_INFO_MEM),
                 (unsigned long)dram_free, (unsigned long)psram_free);
    } else {
        snprintf(s_rec_info[4], sizeof(s_rec_info[4]), "%s: %luK",
                 lang_get(STR_REC_INFO_MEM), (unsigned long)dram_free);
    }
}

/* ========== 刷新：只改文本/样式，不创建/删除对象 ========== */
static void rec_refresh_menu(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    for (int i = 0; i < REC_VIS_ROWS; i++) {
        int idx = s_rec_scroll + i;
        lv_obj_t *row = s_rec_rows[i];
        if (!row) continue;
        if (idx >= REC_ITEM_COUNT) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        char buf[96];
        if (s_rec_confirm == idx && idx == s_rec_sel) {
            snprintf(buf, sizeof(buf), "> %s [%s]",
                     rec_item_label(idx), lang_get(STR_REC_CONFIRM));
        } else {
            snprintf(buf, sizeof(buf), "%s%s",
                     idx == s_rec_sel ? "> " : "  ", rec_item_label(idx));
        }
        if (s_rec_labels[i]) lv_label_set_text(s_rec_labels[i], buf);
        if (idx == s_rec_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
    }
}

static void rec_refresh_info(void)
{
    for (int i = 0; i < REC_VIS_ROWS; i++) {
        lv_obj_t *row = s_rec_rows[i];
        if (!row) continue;
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        if (s_rec_labels[i]) lv_label_set_text(s_rec_labels[i], s_rec_info[i]);
    }
}

static void rec_refresh(void)
{
    if (!s_rec_list) return;
    if (s_rec_view == 1) {
        rec_refresh_info();
    } else {
        rec_refresh_menu();
    }

    /* 状态/提示行 */
    if (s_rec_status_obj) {
        if (s_rec_status[0] && (lv_tick_get() - s_rec_status_tick) < 6000) {
            lv_label_set_text(s_rec_status_obj, s_rec_status);
        } else {
            s_rec_status[0] = '\0';
            lv_label_set_text(s_rec_status_obj, lang_get(STR_REC_HINT));
        }
    }
}

/* ========== 操作执行 ========== */
static void rec_execute(int idx)
{
    switch (idx) {
    case REC_ITEM_FACTORY_RESET: {
        /* 清空 xiaomiao 命名空间的所有 NVS 键值（亮度/音量/语言/字库等） */
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_erase_all(h);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        rec_set_status(err == ESP_OK ? lang_get(STR_REC_FACTORY_DONE)
                                     : lang_get(STR_REC_FAIL));
        break;
    }
    case REC_ITEM_CLEAR_WIFI: {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_WIFI_SSID);
            nvs_erase_key(h, NVS_KEY_WIFI_PASS);
            err = nvs_commit(h);
            nvs_close(h);
        }
        rec_set_status(err == ESP_OK ? lang_get(STR_REC_WIFI_DONE)
                                     : lang_get(STR_REC_FAIL));
        break;
    }
    case REC_ITEM_OTA_RESTART:
        /* ota_start() 是 app_ota.c 内部 static 函数，未导出；进入 OTA 页面后
         * 按 A 即为「重新开始 OTA 升级」，既满足需求又不触碰他人文件。 */
        rec_set_status(lang_get(STR_REC_OTA_HINT));
        ui_stack_push(PAGE_APP_PLACEHOLDER, &g_ota_callbacks, NULL);
        break;
    case REC_ITEM_SD_FLASH: {
        /* 仅检测并提示，不直接刷写分区，避免误操作导致系统不可用 */
        if (!sys_sdcard_is_mounted()) {
            rec_set_status(lang_get(STR_REC_SD_UNMOUNTED));
            break;
        }
        static const char *names[] = {
            "firmware.bin", "update.bin", "ota.bin", "xiaomiao.bin"
        };
        char path[80];
        char found[32] = {0};
        bool found_any = false;
        for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
            snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/%s", names[i]);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(found, sizeof(found), "%s", names[i]);
                found_any = true;
                break;
            }
        }
        if (found_any) {
            rec_set_status("%s: %s | %s", lang_get(STR_REC_SD_FOUND),
                           found, lang_get(STR_REC_OTA_HINT));
        } else {
            rec_set_status("%s | %s", lang_get(STR_REC_SD_MOUNTED),
                           lang_get(STR_REC_SD_NOFW));
        }
        break;
    }
    case REC_ITEM_SYS_INFO:
        rec_collect_info();
        s_rec_view = 1;
        rec_set_status(lang_get(STR_REC_HINT));
        break;
    case REC_ITEM_REBOOT_MAIN: {
        /* 主系统 = launcher(ota_0)。
         * 严格遵守用户约束：绝不在任何路径把下次启动分区设到 factory。
         * 只按 label "launcher" 查 ota_0（app_ota.c 依赖此 label）；
         * 万一找不到，最多按 subtype 再找一次 ota_0，仍找不到则提示失败，
         * 不回退到 factory，也不改动启动分区。 */
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "launcher");
        if (!part) {
            part = esp_partition_find_first(
                ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        }
        if (part && esp_ota_set_boot_partition(part) == ESP_OK) {
            rec_set_status(lang_get(STR_REC_REBOOTING));
            esp_restart();
        } else {
            rec_set_status(lang_get(STR_REC_FAIL));
        }
        break;
    }
    case REC_ITEM_REBOOT_LOADER:
        /* 唯一会设置下次启动分区为 factory 的入口，且仅在此处：
         * 用户进入本页 → 选中该项 → 连按两次 A 显式确认后才会执行。
         * 本页 init / 定时器回调 / 任何自动流程均不调用此函数。 */
        rec_set_status(lang_get(STR_REC_REBOOTING));
        return_to_loader_setup();   /* 设置下次启动到 factory 分区（仅显式操作） */
        esp_restart();
        break;
    default:
        break;
    }
}

/* ========== 生命周期 ========== */
static void rec_timer_cb(lv_timer_t *timer);  /* 前向声明：init 中注册定时器时使用 */

static void rec_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Recovery mode init");

    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_RECOVERY));

    /* 小屏紧凑：字体上限 16px，行高 = 字体 + 2 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 16) font_px = 16;
    s_rec_row_h = font_px + 2;

    s_rec_list = lv_obj_create(scr);
    if (!s_rec_list) {
        ESP_LOGE(TAG, "lv_obj_create(list) failed! mem=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_rec_list);
    lv_obj_set_pos(s_rec_list, 0, ui_content_y());
    lv_obj_set_size(s_rec_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_rec_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 固定创建可视行，之后只改文本，避免定时器内 create/delete */
    memset(s_rec_rows, 0, sizeof(s_rec_rows));
    memset(s_rec_labels, 0, sizeof(s_rec_labels));
    for (int i = 0; i < REC_VIS_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_rec_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row %d) failed! mem=%lu", i,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_rec_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_rec_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(row %d) failed! mem=%lu", i,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 12);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_rec_rows[i] = row;
        s_rec_labels[i] = lbl;
    }

    /* 底部状态/提示行 */
    s_rec_status_obj = lv_label_create(s_rec_list);
    if (s_rec_status_obj) {
        lv_obj_set_style_text_color(s_rec_status_obj, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(s_rec_status_obj, lv_font_cn_get(font_px), 0);
        lv_obj_set_pos(s_rec_status_obj, 0, REC_VIS_ROWS * s_rec_row_h + 1);
        lv_obj_set_width(s_rec_status_obj, LCD_H_RES - 8);
        lv_label_set_long_mode(s_rec_status_obj, LV_LABEL_LONG_SCROLL_CIRCULAR);
    } else {
        ESP_LOGW(TAG, "status label create failed, mem=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }

    s_rec_sel = 0;
    s_rec_scroll = 0;
    s_rec_confirm = -1;
    s_rec_view = 0;
    s_rec_status[0] = '\0';
    memset(s_rec_info, 0, sizeof(s_rec_info));
    rec_refresh();
    ui_dock_create(scr, 1, 0);

    if (!s_rec_timer) {
        s_rec_timer = lv_timer_create(rec_timer_cb, 500, NULL);
    }
}

static void rec_destroy(void)
{
    ESP_LOGI(TAG, "Recovery mode destroy");
    if (s_rec_timer) {
        lv_timer_del(s_rec_timer);
        s_rec_timer = NULL;
    }
    s_rec_list = NULL;
    s_rec_status_obj = NULL;
    memset(s_rec_rows, 0, sizeof(s_rec_rows));
    memset(s_rec_labels, 0, sizeof(s_rec_labels));
    s_rec_status[0] = '\0';
    s_rec_confirm = -1;
    s_rec_view = 0;
}

static void rec_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 只更新文本与高亮，绝不在此 create/delete 对象 */
    rec_refresh();
}

static bool rec_on_key(int key)
{
    if (key == KEY_B) {
        if (s_rec_view == 1) {          /* 系统信息视图 → 返回菜单 */
            s_rec_view = 0;
            rec_refresh();
            return true;
        }
        if (s_rec_confirm >= 0) {       /* 取消二次确认 */
            s_rec_confirm = -1;
            rec_set_status(lang_get(STR_REC_CANCELED));
            rec_refresh();
            return true;
        }
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (s_rec_view == 1) return false;  /* 信息视图仅响应 B */

    if (key == KEY_UP) {
        s_rec_confirm = -1;
        s_rec_sel = (s_rec_sel - 1 + REC_ITEM_COUNT) % REC_ITEM_COUNT;
        rec_scroll_to_sel();
        rec_refresh();
        return true;
    }
    if (key == KEY_DOWN) {
        s_rec_confirm = -1;
        s_rec_sel = (s_rec_sel + 1) % REC_ITEM_COUNT;
        rec_scroll_to_sel();
        rec_refresh();
        return true;
    }
    if (key == KEY_A) {
        if (s_rec_confirm == s_rec_sel) {
            /* 第二次 A：执行操作 */
            s_rec_confirm = -1;
            rec_execute(s_rec_sel);
        } else {
            /* 第一次 A：进入二次确认 */
            s_rec_confirm = s_rec_sel;
            rec_set_status(lang_get(STR_REC_CONFIRM));
        }
        rec_refresh();
        return true;
    }
    return false;
}

const page_callbacks_t g_recovery_callbacks = {
    .init = rec_init,
    .destroy = rec_destroy,
    .on_key = rec_on_key,
};
