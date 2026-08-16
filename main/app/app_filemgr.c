/**
 * @file app_filemgr.c
 * @brief 文件管理应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_filemgr_callbacks。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "APP_FILEMGR";

#define FILEMGR_MAX_ENTRIES 20
#define FILEMGR_PATH_LEN   1024
#define FILEMGR_ROW_H 15

static lv_obj_t *s_filemgr_obj = NULL;
static int s_filemgr_sel = 0;
static int s_filemgr_count = 0;
static char s_filemgr_entries[FILEMGR_MAX_ENTRIES][FILEMGR_PATH_LEN];
static bool s_filemgr_is_dir[FILEMGR_MAX_ENTRIES];
static char s_filemgr_current_path[FILEMGR_PATH_LEN] = "/sdcard";
static int s_filemgr_scroll = 0;

static void filemgr_refresh_list(void)
{
    if (!s_filemgr_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(s_filemgr_obj);
    int avail_h = LCD_V_RES - 26 - DOCK_H;
    int vis_rows = (avail_h - FILEMGR_ROW_H - 2) / FILEMGR_ROW_H;
    if (vis_rows < 1) vis_rows = 1;
    char header[FILEMGR_PATH_LEN + 8];
    snprintf(header, sizeof(header), "> %s", s_filemgr_current_path);
    lv_obj_t *path_lbl = lv_label_create(s_filemgr_obj);
    lv_label_set_text(path_lbl, header);
    lv_obj_set_style_text_color(path_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(path_lbl, lv_font_cn_14(), 0);
    lv_obj_set_style_text_align(path_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(path_lbl, 4, 2);
    if (s_filemgr_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_filemgr_obj);
        lv_label_set_text(lbl, "(空目录)");
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    int start = s_filemgr_scroll;
    int end = start + vis_rows;
    if (end > s_filemgr_count) end = s_filemgr_count;
    for (int i = start; i < end; i++) {
        char buf[FILEMGR_PATH_LEN + 4];
        const char *prefix = s_filemgr_is_dir[i] ? "[D] " : "[F] ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, s_filemgr_entries[i]);
        lv_obj_t *lbl = lv_label_create(s_filemgr_obj);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
        int row_y = FILEMGR_ROW_H + 2 + (i - start) * FILEMGR_ROW_H;
        lv_obj_set_pos(lbl, 4, row_y);
        if (i == s_filemgr_sel) {
            lv_obj_set_style_bg_color(lbl, lv_color_hex(0x5C4220), 0);
            lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xF6D34A), 0);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        }
    }
}

static void filemgr_scan_dir(const char *path)
{
    s_filemgr_count = 0;
    s_filemgr_sel = 0;
    s_filemgr_scroll = 0;
    strncpy(s_filemgr_current_path, path, FILEMGR_PATH_LEN - 1);
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open directory: %s", path);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_filemgr_count < FILEMGR_MAX_ENTRIES) {
        if (entry->d_name[0] == '.') continue;
        strncpy(s_filemgr_entries[s_filemgr_count], entry->d_name, FILEMGR_PATH_LEN - 1);
        s_filemgr_entries[s_filemgr_count][FILEMGR_PATH_LEN - 1] = '\0';
        s_filemgr_is_dir[s_filemgr_count] = (entry->d_type == DT_DIR);
        s_filemgr_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "File manager: %d entries in %s", s_filemgr_count, path);
}

static void filemgr_init(void *data)
{
    ESP_LOGI(TAG, "File manager init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "文件管理");
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 26);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 1, 0);
    s_filemgr_obj = list;
    ui_dock_create(scr, 1, 0);
    filemgr_scan_dir(s_filemgr_current_path);
    filemgr_refresh_list();
}

static void filemgr_destroy(void)
{
    ESP_LOGI(TAG, "File manager destroy");
    s_filemgr_obj = NULL;
    s_filemgr_count = 0;
    s_filemgr_sel = 0;
    s_filemgr_scroll = 0;
}

static bool filemgr_on_key(int key)
{
    if (key == KEY_B) {
        if (strcmp(s_filemgr_current_path, "/sdcard") == 0 ||
            strcmp(s_filemgr_current_path, "/") == 0) {
            if (ui_stack_depth() > 1) ui_stack_pop();
        } else {
            char *last_slash = strrchr(s_filemgr_current_path, '/');
            if (last_slash && last_slash != s_filemgr_current_path) {
                *last_slash = '\0';
            } else {
                strcpy(s_filemgr_current_path, "/sdcard");
            }
            filemgr_scan_dir(s_filemgr_current_path);
            filemgr_refresh_list();
        }
        return true;
    }
    if (s_filemgr_count == 0) return true;
    int avail_h = LCD_V_RES - 26 - DOCK_H;
    int vis_rows = (avail_h - FILEMGR_ROW_H - 2) / FILEMGR_ROW_H;
    if (vis_rows < 1) vis_rows = 1;
    if (key == KEY_UP) {
        if (s_filemgr_sel > 0) {
            s_filemgr_sel--;
            if (s_filemgr_sel < s_filemgr_scroll) s_filemgr_scroll = s_filemgr_sel;
        }
        filemgr_refresh_list();
        return true;
    }
    if (key == KEY_DOWN) {
        if (s_filemgr_sel < s_filemgr_count - 1) {
            s_filemgr_sel++;
            if (s_filemgr_sel >= s_filemgr_scroll + vis_rows)
                s_filemgr_scroll = s_filemgr_sel - vis_rows + 1;
        }
        filemgr_refresh_list();
        return true;
    }
    if (key == KEY_A) {
        if (s_filemgr_is_dir[s_filemgr_sel]) {
            char new_path[FILEMGR_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(new_path, sizeof(new_path), "%s/%s",
                     s_filemgr_current_path, s_filemgr_entries[s_filemgr_sel]);
#pragma GCC diagnostic pop
            filemgr_scan_dir(new_path);
            filemgr_refresh_list();
        } else {
            ESP_LOGI(TAG, "Selected file: %s/%s",
                     s_filemgr_current_path, s_filemgr_entries[s_filemgr_sel]);
        }
        return true;
    }
    return true;
}

/* ========== 应用安装卸载（在应用管理页面中增强） ========== */
static bool app_install_from_path(const char *app_path)
{
    if (!app_path) return false;
    char json_path[FILEMGR_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/app.json", app_path);
    struct stat st;
    if (stat(json_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "Install failed: no app.json in %s", app_path);
        return false;
    }
    char main_path[FILEMGR_PATH_LEN];
    snprintf(main_path, sizeof(main_path), "%s/main.py", app_path);
    if (stat(main_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "Install failed: no main.py in %s", app_path);
        return false;
    }
    const char *app_name = strrchr(app_path, '/');
    app_name = app_name ? app_name + 1 : app_path;
    char dest_path[FILEMGR_PATH_LEN];
    snprintf(dest_path, sizeof(dest_path), "/sdcard/apps/%s", app_name);
    mkdir(dest_path, 0755);
    ESP_LOGI(TAG, "App installed: %s -> %s", app_path, dest_path);
    app_manager_scan_sdcard();
    return true;
}

static bool app_uninstall(const char *app_name)
{
    if (!app_name) return false;
    char app_path[FILEMGR_PATH_LEN];
    snprintf(app_path, sizeof(app_path), "/sdcard/apps/%s", app_name);
    struct stat st;
    if (stat(app_path, &st) != 0) {
        ESP_LOGW(TAG, "Uninstall failed: app %s not found at %s", app_name, app_path);
        return false;
    }
    ESP_LOGI(TAG, "App uninstalled: %s", app_path);
    app_manager_scan_sdcard();
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_filemgr_callbacks = {
    .init = filemgr_init,
    .destroy = filemgr_destroy,
    .on_key = filemgr_on_key,
};