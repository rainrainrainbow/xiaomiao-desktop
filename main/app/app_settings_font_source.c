/**
 * @file app_settings_font_source.c
 * @brief 字库选择二级页面 - 自动扫描SD卡字体文件
 *
 * 自动扫描 /sdcard/Fonts/ 目录下的 .ttf/.otf 文件，
 * 显示可用字体列表供用户选择，支持"内置(英文)"选项。
 * 用户选择后保存字体路径索引到 NVS。
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include "system/sys_nvs.h"
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static const char *TAG = "APP_FONT_SRC";

/* 最大扫描字体数 */
#define FONT_SCAN_MAX 16

/* 字体路径缓冲区 */
static char s_font_paths[FONT_SCAN_MAX][128];
static int s_font_count = 0;

/* 选项总数（字体文件数 + 1个内置选项） */
static int s_total_options = 0;

/* ========== UI状态 ========== */
static lv_obj_t *s_font_src_list = NULL;
static lv_obj_t *s_font_src_labels[FONT_SCAN_MAX + 1] = {0};
static lv_obj_t *s_font_src_checkboxes[FONT_SCAN_MAX] = {0};
static lv_obj_t *s_font_src_status = NULL;   /* 底部状态提示 */
static int s_font_src_sel = 0;
static int s_font_src_scroll = 0;
static int s_font_src_vis_rows = 6;
static int s_font_src_row_h = 14;

/* 显示底部状态提示（"已切换/加载失败"），绿色=成功 红色=失败 */
static void font_src_show_status(const char *msg, bool ok)
{
    if (!s_font_src_status) return;
    lv_label_set_text(s_font_src_status, msg);
    lv_obj_set_style_text_color(s_font_src_status,
        lv_color_hex(ok ? 0x22C55E : 0xEF4444), 0);  /* 绿=成功 红=失败 */
    lv_obj_clear_flag(s_font_src_status, LV_OBJ_FLAG_HIDDEN);
}

/* 刷新复选框状态 */
static void font_src_refresh_checkboxes(void)
{
    ui_state_t *st = ui_state_get();
    int cur_idx = sys_nvs_load_font_path(); /* 0=内置, 1+=字体索引+1 */
    for (int i = 0; i < s_font_count; i++) {
        if (s_font_src_checkboxes[i]) {
            if ((i + 1) == cur_idx) {
                lv_obj_add_state(s_font_src_checkboxes[i], LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_font_src_checkboxes[i], LV_STATE_CHECKED);
            }
        }
    }
}

static void font_src_rebuild_visible(void)
{
    if (!s_font_src_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_font_src_list);
    memset(s_font_src_labels, 0, sizeof(s_font_src_labels));
    memset(s_font_src_checkboxes, 0, sizeof(s_font_src_checkboxes));

    /* 确保选中项在可见范围内 */
    if (s_font_src_sel < s_font_src_scroll) s_font_src_scroll = s_font_src_sel;
    if (s_font_src_sel >= s_font_src_scroll + s_font_src_vis_rows) {
        s_font_src_scroll = s_font_src_sel - s_font_src_vis_rows + 1;
    }
    if (s_font_src_scroll > s_total_options - s_font_src_vis_rows) {
        s_font_src_scroll = s_total_options - s_font_src_vis_rows;
    }
    if (s_font_src_scroll < 0) s_font_src_scroll = 0;

    for (int i = 0; i < s_font_src_vis_rows && (s_font_src_scroll + i) < s_total_options; i++) {
        int idx = s_font_src_scroll + i;
        lv_obj_t *row = lv_obj_create(s_font_src_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_font_src_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_font_src_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (idx == s_font_src_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        if (idx < s_font_count) {
            /* 字体文件项：使用复选框 */
            lv_obj_t *cb = lv_checkbox_create(row);
            /* 提取文件名（不含路径） */
            const char *fname = strrchr(s_font_paths[idx], '/');
            if (fname) fname++; else fname = s_font_paths[idx];
            lv_checkbox_set_text(cb, fname);
            lv_obj_set_style_text_color(cb, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(cb, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(cb, LV_ALIGN_LEFT_MID, 6, 0);
            /* 选中状态 */
            int cur_idx = sys_nvs_load_font_path();
            if ((idx + 1) == cur_idx) {
                lv_obj_add_state(cb, LV_STATE_CHECKED);
            }
            s_font_src_checkboxes[idx] = cb;
            s_font_src_labels[idx] = lv_label_create(row); /* 占位 */
        } else {
            /* 内置(英文)选项 */
            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, lang_get(STR_FONT_SOURCE_BUILTIN));
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
            s_font_src_labels[idx] = lbl;
        }
    }
}

/* 获取文件名中最后一个非路径部分 */
static const char* get_filename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ========== 页面生命周期 ========== */
static void font_src_settings_init(void *data)
{
    ESP_LOGI(TAG, "Font source settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_FONT_SOURCE));

    /* 扫描字体文件 */
    s_font_count = lv_freetype_font_scan(s_font_paths, FONT_SCAN_MAX);
    s_total_options = s_font_count + 1; /* 字体文件 + 内置(英文) */

    ESP_LOGI(TAG, "Font scan: %d fonts found, total options: %d", s_font_count, s_total_options);

    /* 计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_font_src_row_h = font_px + 2;
    s_font_src_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_font_src_row_h;
    if (s_font_src_vis_rows < 1) s_font_src_vis_rows = 1;

    s_font_src_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_font_src_list);
    lv_obj_set_pos(s_font_src_list, 0, ui_content_y());
    lv_obj_set_size(s_font_src_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 12);
    lv_obj_clear_flag(s_font_src_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 底部状态提示标签 */
    s_font_src_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_font_src_status, lv_font_cn_get(font_px), 0);
    lv_obj_set_style_text_color(s_font_src_status, lv_color_hex(0x22C55E), 0);
    lv_obj_set_width(s_font_src_status, LCD_H_RES - 4);
    lv_label_set_long_mode(s_font_src_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_font_src_status, 2, LCD_V_RES - DOCK_H - 12);
    lv_obj_add_flag(s_font_src_status, LV_OBJ_FLAG_HIDDEN);

    /* 找到当前选中的选项 */
    int cur_idx = sys_nvs_load_font_path(); /* 0=内置, 1+=字体索引+1 */
    s_font_src_sel = (cur_idx == 0) ? (s_total_options - 1) : (cur_idx - 1);
    if (s_font_src_sel >= s_total_options) s_font_src_sel = 0;
    s_font_src_scroll = 0;

    font_src_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void font_src_settings_destroy(void)
{
    ESP_LOGI(TAG, "Font source settings destroy");
    s_font_src_list = NULL;
    s_font_src_status = NULL;
    memset(s_font_src_labels, 0, sizeof(s_font_src_labels));
    memset(s_font_src_checkboxes, 0, sizeof(s_font_src_checkboxes));
}

static bool font_src_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();

    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (key == KEY_UP) {
        s_font_src_sel = (s_font_src_sel - 1 + s_total_options) % s_total_options;
        font_src_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_font_src_sel = (s_font_src_sel + 1) % s_total_options;
        font_src_rebuild_visible();
        return true;
    }

    if (key == KEY_A) {
        if (s_font_src_sel < s_font_count) {
            /* 选择了某个字体文件 */
            int new_idx = s_font_src_sel + 1; /* 1-based索引，0=内置 */
            int old_idx = sys_nvs_load_font_path();
            if (new_idx != old_idx) {
                /* 先尝试加载新字体，成功才保存（失败则回滚，避免系统字体瘫痪） */
                lv_result_t res = lv_freetype_font_load_path(s_font_paths[s_font_src_sel]);
                if (res == LV_RESULT_OK) {
                    sys_nvs_save_font_path(new_idx);
                    sys_nvs_save_font_source(0); /* 立即持久化 FreeType 模式，重启后生效 */
                    st->font_source = 0;
                    ESP_LOGI(TAG, "Font changed to: %s", get_filename(s_font_paths[s_font_src_sel]));
                    /* 新字体已加载，返回桌面触发全 UI 重建，立即生效 */
                    ui_stack_back_home();
                    return true;
                } else {
                    ESP_LOGE(TAG, "Failed to load font: %s", get_filename(s_font_paths[s_font_src_sel]));
                    /* 回滚：尝试恢复之前使用的字体 */
                    if (old_idx > 0) {
                        char old_paths[FONT_SCAN_MAX][128];
                        int n = lv_freetype_font_scan(old_paths, FONT_SCAN_MAX);
                        if (old_idx - 1 < n) {
                            lv_freetype_font_load_path(old_paths[old_idx - 1]);
                        }
                    } else if (!lv_freetype_font_is_ready()) {
                        lv_freetype_font_init();
                    }
                    char status_buf[64];
                    snprintf(status_buf, sizeof(status_buf), "✗ %s", get_filename(s_font_paths[s_font_src_sel]));
                    font_src_show_status(status_buf, false);
                }
            } else {
                /* 已经选中的字体，再次确认 */
                char status_buf[64];
                snprintf(status_buf, sizeof(status_buf), "✓ %s", get_filename(s_font_paths[s_font_src_sel]));
                font_src_show_status(status_buf, true);
            }
        } else {
            /* 选择了内置(英文) */
            int old_idx = sys_nvs_load_font_path();
            if (old_idx != 0) {
                sys_nvs_save_font_path(0);
                sys_nvs_save_font_source(1); /* 立即持久化内置模式，重启后生效 */
                st->font_source = 1;
                /* 卸载 FreeType 字体，回退到内置 Montserrat 字体 */
                lv_freetype_font_deinit();
                ESP_LOGI(TAG, "Font changed to built-in (English)");
                /* 返回桌面触发全 UI 重建，立即生效 */
                ui_stack_back_home();
                return true;
            }
        }
        font_src_refresh_checkboxes();
        font_src_rebuild_visible();
        return true;
    }

    return false;
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_font_source_settings_callbacks = {
    .init = font_src_settings_init,
    .destroy = font_src_settings_destroy,
    .on_key = font_src_settings_on_key,
};