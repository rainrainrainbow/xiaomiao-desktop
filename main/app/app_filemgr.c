/**
 * @file app_filemgr.c
 * @brief 文件管理应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_filemgr_callbacks。
 * 功能：浏览目录、打开文本文件、播放MID文件。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "APP_FILEMGR";

#define FILEMGR_MAX_ENTRIES 20
#define FILEMGR_PATH_LEN   256
/* 行高根据字体大小动态计算，在 filemgr_init 中设置 */
static int s_filemgr_row_h = 15;

static lv_obj_t *s_filemgr_obj = NULL;
static int s_filemgr_sel = 0;
static int s_filemgr_count = 0;
static char s_filemgr_entries[FILEMGR_MAX_ENTRIES][FILEMGR_PATH_LEN];
static bool s_filemgr_is_dir[FILEMGR_MAX_ENTRIES];
static char s_filemgr_current_path[FILEMGR_PATH_LEN] = "/sdcard";
static int s_filemgr_scroll = 0;

/* ========== 文本查看器状态 ========== */
#define TXT_MAX_LINES   32
#define TXT_LINE_LEN    80
static lv_obj_t *s_txt_obj = NULL;
static char s_txt_lines[TXT_MAX_LINES][TXT_LINE_LEN];
static int s_txt_line_count = 0;
static int s_txt_scroll = 0;
static int s_txt_viewer_active = 0;

/* ========== 文本查看器 ========== */

static void txt_viewer_open(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open text file: %s", path);
        return;
    }
    s_txt_line_count = 0;
    s_txt_scroll = 0;
    char line[TXT_LINE_LEN];
    while (s_txt_line_count < TXT_MAX_LINES &&
           fgets(line, sizeof(line), fp)) {
        /* 去掉换行符 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        strncpy(s_txt_lines[s_txt_line_count], line, TXT_LINE_LEN - 1);
        s_txt_lines[s_txt_line_count][TXT_LINE_LEN - 1] = '\0';
        s_txt_line_count++;
    }
    fclose(fp);
    s_txt_viewer_active = 1;
    ESP_LOGI(TAG, "Text viewer: %d lines from %s", s_txt_line_count, path);
}

static void txt_viewer_refresh(void)
{
    if (!s_txt_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_txt_obj);
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 1;
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - row_h - 2) / row_h;
    if (vis_rows < 1) vis_rows = 1;
    int start = s_txt_scroll;
    int end = start + vis_rows;
    if (end > s_txt_line_count) end = s_txt_line_count;
    for (int i = start; i < end; i++) {
        int row_idx = i - start;
        lv_obj_t *row = lv_obj_create(s_txt_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_h + 2 + row_idx * row_h);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s_txt_lines[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

static void txt_viewer_close(void)
{
    s_txt_viewer_active = 0;
    s_txt_line_count = 0;
    s_txt_scroll = 0;
    s_txt_obj = NULL;
}

/* ========== 文件打开处理 ========== */

/* 判断是否为文本文件扩展名 */
static bool file_is_text(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".txt") == 0) return true;
    if (strcasecmp(ext, ".py") == 0) return true;
    if (strcasecmp(ext, ".c") == 0) return true;
    if (strcasecmp(ext, ".h") == 0) return true;
    if (strcasecmp(ext, ".cpp") == 0) return true;
    if (strcasecmp(ext, ".json") == 0) return true;
    if (strcasecmp(ext, ".md") == 0) return true;
    if (strcasecmp(ext, ".ini") == 0) return true;
    if (strcasecmp(ext, ".cfg") == 0) return true;
    if (strcasecmp(ext, ".log") == 0) return true;
    if (strcasecmp(ext, ".csv") == 0) return true;
    if (strcasecmp(ext, ".xml") == 0) return true;
    if (strcasecmp(ext, ".html") == 0) return true;
    if (strcasecmp(ext, ".conf") == 0) return true;
    return false;
}

/* 判断是否为MIDI文件 */
static bool file_is_mid(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".mid") == 0) return true;
    if (strcasecmp(ext, ".midi") == 0) return true;
    return false;
}

/* 判断是否为音频文件 */
static bool file_is_audio(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".wav") == 0) return true;
    if (strcasecmp(ext, ".mp3") == 0) return true;
    if (strcasecmp(ext, ".ogg") == 0) return true;
    if (strcasecmp(ext, ".flac") == 0) return true;
    return false;
}

/* 打开文件（根据类型分发） */
static void filemgr_open_file(const char *path, const char *name)
{
    if (file_is_mid(name)) {
        /* 通过 stash 传递文件路径到 MID 播放器 */
        page_stash_t stash;
        size_t path_len = strlen(path);
        stash.valid = true;
        stash.size = (path_len + 1 > PAGE_STASH_SIZE) ? PAGE_STASH_SIZE : path_len + 1;
        memcpy(stash.data, path, stash.size - 1);
        stash.data[stash.size - 1] = '\0';
        ESP_LOGI(TAG, "MID file support removed");
        ui_statusbar_set_title(lang_get(STR_FILE_MID_REMOVED));
    } else if (file_is_text(name)) {
        /* 打开文本查看器 */
        txt_viewer_open(path);
        txt_viewer_refresh();
        ESP_LOGI(TAG, "Opened text file: %s", path);
    } else if (file_is_audio(name)) {
        ESP_LOGW(TAG, "Audio file not supported yet: %s", path);
        /* 状态栏显示提示 */
        ui_statusbar_set_title(lang_get(STR_FILE_AUDIO_NA));
    } else {
        ESP_LOGW(TAG, "Unknown file type: %s", name);
        ui_statusbar_set_title(lang_get(STR_FILE_UNSUPPORTED));
    }
}

static void filemgr_refresh_list(void)
{
    if (!s_filemgr_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_filemgr_obj);
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
    if (vis_rows < 1) vis_rows = 1;
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    /* 路径行 */
    lv_obj_t *path_row = lv_obj_create(s_filemgr_obj);
    lv_obj_remove_style_all(path_row);
    lv_obj_set_pos(path_row, 0, 0);
    lv_obj_set_size(path_row, LCD_H_RES, s_filemgr_row_h);
    lv_obj_clear_flag(path_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(path_row, LV_OPA_TRANSP, 0);
    char header[FILEMGR_PATH_LEN + 8];
    snprintf(header, sizeof(header), "> %s", s_filemgr_current_path);
    lv_obj_t *path_lbl = lv_label_create(path_row);
    lv_label_set_text(path_lbl, header);
    lv_obj_set_style_text_color(path_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(path_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_width(path_lbl, LCD_H_RES - 8);
    lv_label_set_long_mode(path_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(path_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    if (s_filemgr_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_filemgr_obj);
        lv_label_set_text(lbl, lang_get(STR_FILE_EMPTY_DIR));
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int start = s_filemgr_scroll;
    int end = start + vis_rows;
    if (end > s_filemgr_count) end = s_filemgr_count;
    for (int i = start; i < end; i++) {
        int row_idx = i - start + 1; /* 第0行是路径 */
        lv_obj_t *row = lv_obj_create(s_filemgr_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_filemgr_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_filemgr_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_filemgr_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        char buf[FILEMGR_PATH_LEN + 4];
        const char *prefix = s_filemgr_is_dir[i] ? "📁 " : "📄 ";
        snprintf(buf, sizeof(buf), "%s%s", prefix, s_filemgr_entries[i]);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
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
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_FILES));
    
    /* 根据字体大小动态计算行高 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_filemgr_row_h = font_px + 1;  /* 字体高度 + 1px间距 */
    
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_filemgr_obj = list;
    s_txt_obj = list;  /* 文本查看器复用同一内容区 */
    ui_dock_create(scr, 1, 0);
    filemgr_scan_dir(s_filemgr_current_path);
    filemgr_refresh_list();
}

static void filemgr_destroy(void)
{
    ESP_LOGI(TAG, "File manager destroy");
    s_filemgr_obj = NULL;
    s_txt_obj = NULL;
    s_filemgr_count = 0;
    s_filemgr_sel = 0;
    s_filemgr_scroll = 0;
    txt_viewer_close();
}

static bool filemgr_on_key(int key)
{
    /* 文本查看器模式 */
    if (s_txt_viewer_active) {
        if (key == KEY_B) {
            txt_viewer_close();
            s_txt_obj = s_filemgr_obj;
            filemgr_refresh_list();
            return true;
        }
        if (key == KEY_UP) {
            if (s_txt_scroll > 0) s_txt_scroll--;
            txt_viewer_refresh();
            return true;
        }
        if (key == KEY_DOWN) {
            int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
            int font_px = ui_state_get()->font_size;
            if (font_px < 14) font_px = 14;
            if (font_px > 24) font_px = 24;
            int row_h = font_px + 1;
            int vis_rows = (avail_h - row_h - 2) / row_h;
            if (vis_rows < 1) vis_rows = 1;
            if (s_txt_scroll + vis_rows < s_txt_line_count) s_txt_scroll++;
            txt_viewer_refresh();
            return true;
        }
        return true;
    }

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
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
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
            /* 打开文件 */
            char file_path[FILEMGR_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(file_path, sizeof(file_path), "%s/%s",
                     s_filemgr_current_path, s_filemgr_entries[s_filemgr_sel]);
#pragma GCC diagnostic pop
            filemgr_open_file(file_path, s_filemgr_entries[s_filemgr_sel]);
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