/**
 * @file app_micropython.c
 * @brief MicroPython 应用层集成 - 委托给 poincare/runtime 新架构实现
 * 
 * 本文件是旧应用层（main/app）与新架构 Poincaré 运行时（poincare/runtime）的
 * 适配层。MicroPython 核心运行时、脚本执行、NLR jump fail 处理、native code
 * commit 等在 poincare/runtime.c 中统一实现，此处仅保留应用页面回调和扫描逻辑。
 *
 * 这样消除了 app_micropython.c 与 poincare/runtime.c 之间的重复代码和潜在
 * 重复符号（nlr_jump_fail、esp_native_code_commit 等），符合渐进式迁移方向。
 */

#include "app_manager.h"
#include "app_micropython.h"
#include "ui_framework.h"
#include "poincare/runtime.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_PY";

/* ========== MicroPython 运行时 API（委托给 poincare/runtime） ========== */

/**
 * @brief 初始化 MicroPython 运行时（幂等，可重复调用）
 * @return true 成功，false 失败
 */
bool app_micropython_init(void)
{
    /* 委托给 Poincaré 运行时（默认 64KB PSRAM GC 堆） */
    return poincare_runtime_init(0);
}

/**
 * @brief 检查 MicroPython 是否已初始化
 */
bool app_micropython_is_ready(void)
{
    return poincare_runtime_is_ready();
}

/**
 * @brief 执行一段 Python 源码字符串
 */
int app_micropython_exec(const char *source, const char *source_name)
{
    return poincare_runtime_exec(source, source_name ? source_name : "<string>");
}

/**
 * @brief 执行一个 Python 文件
 */
int app_micropython_exec_file(const char *filename)
{
    return poincare_runtime_exec_file(filename);
}

/* ========== MicroPython应用页面回调 ========== */

static void python_app_init(void *data);
static void python_app_activate(void);
static void python_app_destroy(void);
static bool python_app_on_key(int key);

static const page_callbacks_t s_python_callbacks = {
    .init = python_app_init,
    .activate = python_app_activate,
    .destroy = python_app_destroy,
    .on_key = python_app_on_key,
};

/* ========== MicroPython应用初始化 ========== */
static void python_app_init(void *data)
{
    const app_def_t *app = (const app_def_t *)data;
    if (!app) {
        ESP_LOGE(TAG, "NULL app data");
        return;
    }

    ESP_LOGI(TAG, "Python app init: %s", app->name);
    /* 初始化 MicroPython 运行时（委托给 Poincaré） */
    app_micropython_init();
}

static void python_app_activate(void)
{
    ESP_LOGI(TAG, "Python app activate");

    /* 获取当前应用的 app_def_t（从页面栈数据传入） */
    const page_callbacks_t *cbs = ui_stack_current_callbacks();
    if (cbs && cbs->init == python_app_init) {
        // 从页面栈获取应用数据
        // 注意：python_app_init 中 data 参数就是 app_def_t 指针
    }
    
    /* 在屏幕上显示 MicroPython 应用信息 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    
    /* 获取当前应用名（从 app_manager 或标题） */
    const char *app_name = app_manager_get_current_name();
    if (!app_name) app_name = "Python";
    ui_titlebar_create(scr, 14, app_name);

    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);

    /* 查找当前应用的入口文件 */
    int py_count = 0;
    const app_def_t *py_apps = app_manager_get_micropython(&py_count);
    const char *entry_file = NULL;
    
    for (int i = 0; i < py_count; i++) {
        if (strcmp(py_apps[i].name, app_name) == 0) {
            entry_file = py_apps[i].py_entry;
            break;
        }
    }
    
    /* 如果没找到，检查是否是内置的"Python"测试应用 */
    if (!entry_file && strcmp(app_name, "Python") == 0) {
        /* 执行测试脚本 */
        int ret = app_micropython_exec("print('Hello from XiaoMiao MicroPython!')\n", "<boot>");

        /* 显示测试结果 */
        lv_obj_t *lbl = lv_label_create(scr);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -8);

        if (ret == 0) {
            lv_label_set_text(lbl, "MicroPython 运行正常!\nHello from XiaoMiao!");
            ESP_LOGI(TAG, "MicroPython test PASSED");
        } else {
            lv_label_set_text(lbl, "MicroPython 测试失败\n请查看串口日志");
            ESP_LOGE(TAG, "MicroPython test FAILED");
        }
        
        ui_dock_create(scr, 1, 0);
        return;
    }
    
    if (!entry_file) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "未找到入口文件");
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -8);
        ui_dock_create(scr, 1, 0);
        return;
    }

    /* 执行入口文件 */
    ESP_LOGI(TAG, "Executing MicroPython app: %s", entry_file);
    int ret = app_micropython_exec_file(entry_file);

    /* 显示执行结果 */
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -8);

    if (ret == 0) {
        lv_label_set_text(lbl, "应用运行完毕");
        ESP_LOGI(TAG, "MicroPython app %s executed successfully", entry_file);
    } else {
        lv_label_set_text(lbl, "应用执行失败\n请查看串口日志");
        ESP_LOGE(TAG, "MicroPython app %s execution FAILED", entry_file);
    }

    ui_dock_create(scr, 1, 0);
}

static void python_app_destroy(void)
{
    ESP_LOGI(TAG, "Python app destroy");
}

static bool python_app_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) {
            ui_stack_pop();
        }
        return true;
    }
    return false;
}

/* ========== 获取Python应用页面回调 ========== */
const page_callbacks_t* app_micropython_get_callbacks(void)
{
    return &s_python_callbacks;
}

/* ========== 扫描SD卡Python应用 ========== */
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* 编译器警告：snprintf 的 %s 可能被截断，但实际运行时路径长度可控 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* Python 应用目录结构：
 * /sdcard/apps/<app_name>/
 *   ├── app.json     # 应用元数据（名称、图标、颜色等）
 *   └── main.py      # 入口文件
 *
 * app.json 格式：
 * {
 *   "name": "应用名",
 *   "icon": "图标字符（1-2个LVGL符号）",
 *   "color": "图标颜色（十六进制，如"#FF0000"）",
 *   "version": "1.0",
 *   "author": "作者名"
 * }
 *
 * 如果 app.json 不存在，则使用目录名作为应用名，默认图标和颜色。
 */

/* 解析 app.json 中的字符串值 */
static char* json_get_string(const char *json, const char *key, char *buf, int buf_size)
{
    if (!json || !key) return NULL;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"\\s*:\\s*\"", key);
    
    const char *p = json;
    while (*p) {
        // 查找 key
        const char *found = strstr(p, pattern);
        if (!found) return NULL;
        
        const char *val_start = found + strlen(pattern);
        // 找到值结束的引号
        const char *val_end = val_start;
        while (*val_end && *val_end != '"') {
            if (*val_end == '\\') val_end++; // 跳过转义
            val_end++;
        }
        
        int len = val_end - val_start;
        if (len > 0 && len < buf_size) {
            strncpy(buf, val_start, len);
            buf[len] = '\0';
            return buf;
        }
        p = val_end;
    }
    return NULL;
}

/* 解析 app.json 中的十六进制颜色值 */
static uint32_t json_get_color(const char *json, const char *key, uint32_t default_color)
{
    char buf[16];
    if (!json_get_string(json, key, buf, sizeof(buf))) return default_color;
    
    // 支持 "#FF0000" 或 "0xFF0000" 格式
    unsigned long color = 0;
    if (buf[0] == '#') {
        sscanf(buf + 1, "%lx", &color);
    } else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        sscanf(buf + 2, "%lx", &color);
    } else {
        sscanf(buf, "%lx", &color);
    }
    return (uint32_t)(color & 0xFFFFFF);
}

/* 读取文件内容到堆内存 */
static char* read_file_to_heap(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 4096) { // 限制最大 4KB
        fclose(f);
        return NULL;
    }
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read_size = fread(buf, 1, size, f);
    fclose(f);
    buf[read_size] = '\0';
    return buf;
}

int app_micropython_scan(const char *base_path, app_def_t *apps, int max_count)
{
    if (!base_path) base_path = "/sdcard/apps";
    if (!apps || max_count <= 0) return 0;
    
    ESP_LOGI(TAG, "Scanning MicroPython apps in %s", base_path);
    
    DIR *dir = opendir(base_path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open apps directory: %s", base_path);
        return 0;
    }
    
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        // 跳过 . 和 ..
        if (entry->d_name[0] == '.') continue;
        
        // 只处理目录
        if (entry->d_type != DT_DIR) continue;
        
        // 检查 main.py 是否存在
        char main_path[256];
        snprintf(main_path, sizeof(main_path), "%s/%s/main.py", base_path, entry->d_name);
        
        struct stat st;
        if (stat(main_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            ESP_LOGW(TAG, "  [%s] No main.py found, skipping", entry->d_name);
            continue;
        }
        
        // 读取 app.json（可选）
        char json_path[256];
        snprintf(json_path, sizeof(json_path), "%s/%s/app.json", base_path, entry->d_name);
        char *json_content = read_file_to_heap(json_path);
        
        // 填充应用定义
        app_def_t *app = &apps[count];
        memset(app, 0, sizeof(app_def_t));
        
        // 应用名：优先从 app.json 读取，否则使用目录名
        char name_buf[32];
        if (json_content && json_get_string(json_content, "name", name_buf, sizeof(name_buf))) {
            app->name = strdup(name_buf);
        } else {
            app->name = strdup(entry->d_name);
        }
        
        // 图标：优先从 app.json 读取
        char icon_buf[8];
        if (json_content && json_get_string(json_content, "icon", icon_buf, sizeof(icon_buf))) {
            // 使用静态字符串存储图标
            static char s_icon_pool[16][8];
            static int s_icon_idx = 0;
            strncpy(s_icon_pool[s_icon_idx % 16], icon_buf, 7);
            s_icon_pool[s_icon_idx % 16][7] = '\0';
            app->icon_text = s_icon_pool[s_icon_idx % 16];
            s_icon_idx++;
        } else {
            app->icon_text = LV_SYMBOL_COPY; // 默认图标
        }
        
        // 颜色：优先从 app.json 读取
        if (json_content) {
            app->icon_color = json_get_color(json_content, "color", 0x3B82F6);
        } else {
            app->icon_color = 0x3B82F6; // 默认蓝色
        }
        
        app->type = APP_TYPE_MICROPYTHON;
        
        // 入口文件路径
        app->py_entry = strdup(main_path);
        
        if (json_content) free(json_content);
        
        ESP_LOGI(TAG, "  [%d] %s -> %s", count, app->name, app->py_entry);
        count++;
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Found %d MicroPython apps", count);
    return count;
}

#pragma GCC diagnostic pop