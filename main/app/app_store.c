/**
 * @file app_store.c
 * @brief 应用商店 — 浏览和安装MicroPython应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_store_callbacks。
 * 从SD卡扫描MicroPython应用，显示应用列表，支持安装。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "APP_STORE";

/* ========== 常量 ========== */
#define STORE_MAX_APPS      20   // 最大显示应用数
#define STORE_PATH_LEN      256  // 路径缓冲区大小（d_name 最大 255 字节 + 前缀）
#define STORE_SCAN_DIR      "/sdcard"  // 扫描目录（可修改为特定目录）

/* ========== 应用条目 ========== */
typedef struct {
    char name[32];        // 应用名（从目录名提取）
    char path[STORE_PATH_LEN];  // 完整路径
    bool has_app_json;    // 是否有 app.json
    bool has_main_py;     // 是否有 main.py
    bool installed;       // 是否已安装（在 /sdcard/apps/ 下）
} store_app_t;

/* ========== 全局状态 ========== */
static lv_obj_t *s_store_obj = NULL;       // 列表容器
static lv_obj_t *s_info_label = NULL;      // 信息提示标签
static store_app_t s_apps[STORE_MAX_APPS]; // 扫描到的应用列表
static int s_app_count = 0;                // 应用数量
static int s_sel = 0;                      // 选中索引
static int s_scroll = 0;                   // 滚动偏移
static int s_row_h = 16;                   // 行高（根据字体自适应）

/* ========== 扫描SD卡中的应用 ========== */

/** 检查路径是否为目录 */
static bool is_dir(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

/** 检查文件是否存在 */
static bool file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/** 检查应用是否已安装（在 /sdcard/apps/ 下） */
static bool is_app_installed(const char *app_name)
{
    char path[STORE_PATH_LEN];
    snprintf(path, sizeof(path), "/sdcard/apps/%s", app_name);
    return is_dir(path);
}

/** 扫描SD卡目录，查找可能的MicroPython应用 */
static void scan_sdcard_apps(void)
{
    s_app_count = 0;
    
    DIR *dir = opendir(STORE_SCAN_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open %s", STORE_SCAN_DIR);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_app_count < STORE_MAX_APPS) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        // 只检查目录
        char full_path[STORE_PATH_LEN];
        #ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
        snprintf(full_path, sizeof(full_path), "%s/%s", STORE_SCAN_DIR, entry->d_name);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
        if (!is_dir(full_path))
            continue;
        
        // 检查是否有 .app 后缀的目录
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".app") != 0)
            continue;
        
        // 提取应用名（去掉 .app 后缀）
        store_app_t *app = &s_apps[s_app_count];
        size_t name_len = dot - entry->d_name;
        if (name_len >= sizeof(app->name)) name_len = sizeof(app->name) - 1;
        strncpy(app->name, entry->d_name, name_len);
        app->name[name_len] = '\0';
        strncpy(app->path, full_path, sizeof(app->path) - 1);
        app->path[sizeof(app->path) - 1] = '\0';
        
        // 检查 app.json 和 main.py
        char json_path[STORE_PATH_LEN + 16];
        snprintf(json_path, sizeof(json_path), "%s/app.json", full_path);
        app->has_app_json = file_exists(json_path);
        
        char py_path[STORE_PATH_LEN + 16];
        snprintf(py_path, sizeof(py_path), "%s/main.py", full_path);
        app->has_main_py = file_exists(py_path);
        
        app->installed = is_app_installed(app->name);
        
        s_app_count++;
        ESP_LOGI(TAG, "Found app: %s (json=%d, py=%d, installed=%d)",
                 app->name, app->has_app_json, app->has_main_py, app->installed);
    }
    closedir(dir);
    
    ESP_LOGI(TAG, "Scanned %d apps from SD card", s_app_count);
}

/* ========== 安装应用 ========== */

/** 安装选中的应用 */
static void install_selected_app(void)
{
    if (s_sel < 0 || s_sel >= s_app_count) return;
    
    store_app_t *app = &s_apps[s_sel];
    if (app->installed) {
        ESP_LOGI(TAG, "App %s already installed", app->name);
        return;
    }
    
    ESP_LOGI(TAG, "Installing app: %s from %s", app->name, app->path);
    
    // 创建目标目录
    char dest_dir[STORE_PATH_LEN];
    snprintf(dest_dir, sizeof(dest_dir), "/sdcard/apps/%s", app->name);
    mkdir(dest_dir, 0755);
    
    // 复制 app.json
    char src[STORE_PATH_LEN + 16], dst[STORE_PATH_LEN + 16];
    if (app->has_app_json) {
        snprintf(src, sizeof(src), "%s/app.json", app->path);
        snprintf(dst, sizeof(dst), "%s/app.json", dest_dir);
        // 使用文件复制（简化版：通过系统命令）
        char cmd[STORE_PATH_LEN * 2 + 64];
        snprintf(cmd, sizeof(cmd), "cp %s %s", src, dst);
        system(cmd);
    }
    
    // 复制 main.py
    if (app->has_main_py) {
        snprintf(src, sizeof(src), "%s/main.py", app->path);
        snprintf(dst, sizeof(dst), "%s/main.py", dest_dir);
        char cmd[STORE_PATH_LEN * 2 + 64];
        snprintf(cmd, sizeof(cmd), "cp %s %s", src, dst);
        system(cmd);
    }
    
    // 重新扫描应用管理器
    app_manager_scan_sdcard();
    
    // 更新状态
    app->installed = true;
    ESP_LOGI(TAG, "App %s installed successfully", app->name);
}

/* ========== 卸载应用 ========== */

static void uninstall_selected_app(void)
{
    if (s_sel < 0 || s_sel >= s_app_count) return;
    
    store_app_t *app = &s_apps[s_sel];
    if (!app->installed) {
        ESP_LOGI(TAG, "App %s is not installed", app->name);
        return;
    }
    
    ESP_LOGI(TAG, "Uninstalling app: %s", app->name);
    
    char dest_dir[STORE_PATH_LEN];
    snprintf(dest_dir, sizeof(dest_dir), "/sdcard/apps/%s", app->name);
    
    // 删除目录
    char cmd[STORE_PATH_LEN * 2];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dest_dir);
    system(cmd);
    
    // 重新扫描应用管理器
    app_manager_scan_sdcard();
    
    // 更新状态
    app->installed = false;
    ESP_LOGI(TAG, "App %s uninstalled successfully", app->name);
}

/* ========== UI渲染 ========== */

/** 重新构建可见行 */
static void store_rebuild_visible(void)
{
    if (!s_store_obj) return;
    
    lv_obj_clean(s_store_obj);
    
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_row_h = font_px + 2;
    if (s_row_h < 16) s_row_h = 16;
    
    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_row_h;
    if (vis_rows < 1) vis_rows = 1;
    
    // 确保选中项可见
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis_rows) s_scroll = s_sel - vis_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_app_count - vis_rows) s_scroll = s_app_count - vis_rows;
    if (s_scroll < 0) s_scroll = 0;
    
    for (int i = 0; i < vis_rows && (i + s_scroll) < s_app_count; i++) {
        int idx = i + s_scroll;
        store_app_t *app = &s_apps[idx];
        
        lv_obj_t *row = lv_obj_create(s_store_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        
        // 选中高亮
        if (idx == s_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        
        // 应用名
        lv_obj_t *name_lbl = lv_label_create(row);
        if (name_lbl) {
            lv_obj_set_style_text_font(name_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_width(name_lbl, LCD_H_RES - 70);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 2, 0);
            lv_label_set_text(name_lbl, app->name);
        }
        
        // 状态标签（已安装/未安装）
        lv_obj_t *status_lbl = lv_label_create(row);
        if (status_lbl) {
            lv_obj_set_style_text_font(status_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(status_lbl, 
                lv_color_hex(app->installed ? 0x22C55E : colors->text_dim), 0);
            lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -2, 0);
            lv_label_set_text(status_lbl, app->installed ? lang_get(STR_STORE_INSTALLED) : lang_get(STR_STORE_NOT_INSTALLED));
        }
    }
    
    // 如果没有应用，显示提示
    if (s_app_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_store_obj);
        if (empty_lbl) {
            lv_obj_set_style_text_font(empty_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(empty_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
            lv_label_set_text(empty_lbl, lang_get(STR_STORE_EMPTY));
        }
    }
}

/** 更新信息标签 */
static void update_info_label(void)
{
    if (!s_info_label) return;
    const theme_colors_t *colors = ui_theme_colors();
    
    if (s_app_count > 0 && s_sel >= 0 && s_sel < s_app_count) {
        store_app_t *app = &s_apps[s_sel];
        char buf[64];
        snprintf(buf, sizeof(buf), "%s | %s%s",
                 app->installed ? lang_get(STR_STORE_UNINSTALL) : lang_get(STR_STORE_INSTALL),
                 app->has_app_json ? "✓json" : "✗json",
                 app->has_main_py ? " ✓py" : " ✗py");
        lv_label_set_text(s_info_label, buf);
    } else {
        lv_label_set_text(s_info_label, "");
    }
}

/* ========== 页面生命周期回调 ========== */

static void store_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Store init");
    
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_STORE));
    
    // 计算行高
    s_row_h = ui_state_get()->font_size + 2;
    if (s_row_h < 16) s_row_h = 16;
    
    // 列表容器
    s_store_obj = lv_obj_create(scr);
    if (!s_store_obj) {
        ESP_LOGE(TAG, "lv_obj_create(store_obj) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_store_obj);
    lv_obj_set_pos(s_store_obj, 0, ui_content_y());
    lv_obj_set_size(s_store_obj, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 12);
    lv_obj_clear_flag(s_store_obj, LV_OBJ_FLAG_SCROLLABLE);
    
    // 底部信息栏
    s_info_label = lv_label_create(scr);
    if (!s_info_label) {
        ESP_LOGE(TAG, "lv_label_create(info_label) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lv_obj_set_style_text_font(s_info_label, lv_font_cn_get(ui_state_get()->font_size), 0);
        lv_obj_set_style_text_color(s_info_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_width(s_info_label, LCD_H_RES - 4);
        lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(s_info_label, 2, LCD_V_RES - DOCK_H - 12);
    }
    
    // 底部导航栏
    ui_dock_create(scr, 1, 0);
    
    // 扫描应用
    s_sel = 0;
    s_scroll = 0;
    scan_sdcard_apps();
    store_rebuild_visible();
    update_info_label();
}

static void store_destroy(void)
{
    ESP_LOGI(TAG, "Store destroy");
    s_store_obj = NULL;
    s_info_label = NULL;
}

static void store_activate(void)
{
    ESP_LOGI(TAG, "Store activate");
    // 重新扫描（可能有新安装的应用）
    scan_sdcard_apps();
    store_rebuild_visible();
    update_info_label();
}

static bool store_on_key(int key)
{
    switch (key) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_DOWN:
            if (s_sel < s_app_count - 1) {
                s_sel++;
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_A:
            if (s_app_count > 0 && s_sel >= 0 && s_sel < s_app_count) {
                store_app_t *app = &s_apps[s_sel];
                if (app->installed) {
                    uninstall_selected_app();
                } else {
                    install_selected_app();
                }
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_B:
            if (ui_stack_depth() > 1) ui_stack_pop();
            return true;
        default:
            return true;
    }
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_store_callbacks = {
    .init = store_init,
    .activate = store_activate,
    .destroy = store_destroy,
    .on_key = store_on_key,
};