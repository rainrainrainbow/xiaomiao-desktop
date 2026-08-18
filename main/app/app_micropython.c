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
#include "poincare/mp_xiaomiao.h"
#include "driver/drv_button.h"   /* drv_button_get_event 供注入 */
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
// FreeType 字体支持（统一中文字体入口）
#include "fonts/lv_freetype_font.h"
// FreeRTOS 任务
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_PY";

/* ========== 按键读取回调适配（main → micropython） ========== */
/* 与 xm_btn_event_t 字段布局相同（key + is_long_press），安全转指针 */
static bool py_btn_read_adapter(xm_btn_event_t *evt)
{
    return drv_button_get_event((btn_event_t *)evt);
}

/* ========== MicroPython 运行时 API（委托给 poincare/runtime） ========== */

/**
 * @brief 初始化 MicroPython 运行时（幂等，可重复调用）
 * @return true 成功，false 失败
 */
bool app_micropython_init(void)
{
    /* 委托给 Poincaré 运行时（默认 64KB PSRAM GC 堆） */
    bool ok = poincare_runtime_init(0);
    if (ok) {
        /* 注入按键读取回调：main 组件提供 drv_button_get_event */
        xiaomiao_button_set_read_cb(py_btn_read_adapter);
    }
    return ok;
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

/* ========== Python 应用运行支持（v66：屏幕/按键/时间绑定） ========== */

/* Python 任务句柄与运行标志 */
static TaskHandle_t s_py_task = NULL;
static volatile bool s_py_task_running = false;
static volatile bool s_py_running_app = false;   /* 当前是否在运行 Python 应用页面 */
static char s_py_entry_path[256] = {0};          /* 当前 Python 入口文件 */

/* flush 脏标志：Python 调用 show() 时置位，main 循环 app_micropython_on_tick 消费 */
static volatile bool s_py_flush_pending = false;

/* LVGL canvas：承接 framebuffer（零拷贝） */
static lv_obj_t *s_py_canvas = NULL;

/* Python 任务 flush 回调（在 Python 任务上下文调用，仅置脏标志） */
static void py_flush_cb(void)
{
    s_py_flush_pending = true;
}

/* Python 运行任务：执行 main.py（脚本内含游戏循环），结束后退出 */
static void py_run_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Python task started: %s", s_py_entry_path);
    s_py_task_running = true;

    /* 清空转发队列，避免残留旧按键 */
    xiaomiao_button_flush();

    /* 执行入口脚本（阻塞直到脚本返回） */
    int ret = app_micropython_exec_file(s_py_entry_path);
    ESP_LOGI(TAG, "Python task finished: %s (ret=%d)", s_py_entry_path, ret);

    s_py_task_running = false;
    s_py_task = NULL;

    /* 任务自删除 */
    vTaskDelete(NULL);
}

/* main 循环周期调用：将 Python framebuffer 刷新到屏幕 */
void app_micropython_on_tick(void)
{
    if (!s_py_running_app || !s_py_canvas) return;

    if (s_py_flush_pending) {
        s_py_flush_pending = false;
        /* 在 main 任务上下文操作 LVGL：无效化 canvas 并强制刷新 */
        lv_obj_invalidate(s_py_canvas);
        lv_refr_now(NULL);
        ESP_LOGV(TAG, "Python frame flushed");
    }
}

/* 停止当前 Python 应用：请求协作式停止 + 复位 canvas/标志 */
static void python_app_stop_running(void)
{
    if (s_py_task_running) {
        ESP_LOGI(TAG, "Requesting Python app stop...");
        xiaomiao_request_stop();
        /* 等待任务退出（脚本响应 KeyboardInterrupt 后自然结束） */
        for (int i = 0; i < 50 && s_py_task_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));  /* 最多等 1 秒 */
        }
        if (s_py_task_running) {
            ESP_LOGW(TAG, "Python task did not exit in time, abandoning handle");
        }
        s_py_task = NULL;
    }
    s_py_running_app = false;
    s_py_canvas = NULL;
    s_py_flush_pending = false;
    xiaomiao_display_set_flush_cb(NULL);  /* 取消 flush 回调 */
}

/* MicroPython应用页面回调 */

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

    /* 获取当前应用名（从 app_manager 或标题） */
    const char *app_name = app_manager_get_current_name();
    if (!app_name) app_name = "Python";

    int font_px = ui_state_get()->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    /* 构建页面：状态栏 + canvas（承接 framebuffer） + dock */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_statusbar_set_title(app_name);

    lv_coord_t content_y = ui_content_y();
    lv_coord_t content_h = LCD_V_RES - content_y - DOCK_H;

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

    /*
     * 核心：Python 应用绘图通过 xiaomiao 模块的 framebuffer（160x128 RGB565 SWAPPED）
     * 这里用一个 LVGL canvas 以零拷贝方式承接 framebuffer 地址，
     * Python 脚本调用 show() 时置脏标志，main 循环 in 每次迭代调用
     * app_micropython_on_tick() 完成 canvas 无效化 + 强制刷新。
     * 这样 Python 游戏画面就能实时上屏，且不阻塞 main 任务。
     */
    uint16_t *fb = xiaomiao_display_get_framebuffer();
    if (fb && entry_file) {
        /* 若已有 Python 应用在运行，先停止旧任务 */
        if (s_py_task_running || s_py_running_app) {
            python_app_stop_running();
        }

        lv_obj_t *canvas = lv_canvas_create(scr);
        lv_obj_remove_style_all(canvas);
        lv_obj_set_pos(canvas, 0, content_y);
        lv_obj_set_size(canvas, LCD_H_RES, content_h);
        lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_canvas_set_buffer(canvas, fb, LCD_H_RES, content_h, LV_COLOR_FORMAT_RGB565_SWAPPED);
        lv_obj_set_style_bg_color(canvas, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);

        s_py_canvas = canvas;
        s_py_running_app = true;

        /* 注册 flush 回调：Python show() → 置脏标志 */
        xiaomiao_display_set_flush_cb(py_flush_cb);

        /* 启动独立任务执行 main.py（不阻塞 UI） */
        strncpy(s_py_entry_path, entry_file, sizeof(s_py_entry_path) - 1);
        s_py_entry_path[sizeof(s_py_entry_path) - 1] = '\0';
        xTaskCreate(py_run_task, "py_app", 16384, NULL, 10, &s_py_task);

        /* 显示启动提示 */
        lv_obj_t *hint = lv_label_create(scr);
        lv_label_set_text(hint, "加载中...");
        lv_obj_set_style_text_color(hint, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint, lv_font_cn_get(font_px), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -DOCK_H - 2);

        ui_dock_create(scr, 1, 0);
        ESP_LOGI(TAG, "Python app started in background task: %s", entry_file);
        return;
    }

    /* 兜底：没有 framebuffer 或入口文件 → 显示传统结果页面 */
    int ret = -1;
    const char *result_msg = NULL;
    const char *result_icon = LV_SYMBOL_OK;
    uint32_t result_color = colors->text;

    /* 如果没找到，检查是否是内置的"Python"测试应用 */
    if (!entry_file && strcmp(app_name, "Python") == 0) {
        /* 执行测试脚本 */
        ret = app_micropython_exec("print('Hello from XiaoMiao MicroPython!')\n", "<boot>");
        if (ret == 0) {
            result_msg = "MicroPython 运行正常!\nHello from XiaoMiao!";
            ESP_LOGI(TAG, "MicroPython test PASSED");
        } else {
            result_msg = "MicroPython 测试失败\n请查看串口日志";
            result_icon = LV_SYMBOL_WARNING;
            result_color = 0xFF4444;
            ESP_LOGE(TAG, "MicroPython test FAILED");
        }
    } else if (!entry_file) {
        result_msg = "未找到入口文件";
        result_icon = LV_SYMBOL_WARNING;
        result_color = 0xFFAA00;
    } else {
        /* 执行入口文件 */
        ESP_LOGI(TAG, "Executing MicroPython app: %s", entry_file);
        ret = app_micropython_exec_file(entry_file);
        if (ret == 0) {
            result_msg = "应用运行完毕";
            ESP_LOGI(TAG, "MicroPython app %s executed successfully", entry_file);
        } else {
            result_msg = "应用执行失败\n请查看串口日志";
            result_icon = LV_SYMBOL_WARNING;
            result_color = 0xFF4444;
            ESP_LOGE(TAG, "MicroPython app %s execution FAILED", entry_file);
        }
    }

    /* 创建内容容器 */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, content_y);
    lv_obj_set_size(content, LCD_H_RES, content_h);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    /* 应用图标（居中上方，使用LVGL内置Montserrat字体显示符号） */
    lv_obj_t *icon_lbl = lv_label_create(content);
    lv_label_set_text(icon_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(icon_lbl, LV_ALIGN_TOP_MID, 0, 8);

    /* 结果状态图标（使用LVGL内置Montserrat字体显示符号） */
    lv_obj_t *result_icon_lbl = lv_label_create(content);
    lv_label_set_text(result_icon_lbl, result_icon);
    lv_obj_set_style_text_color(result_icon_lbl, lv_color_hex(result_color), 0);
    lv_obj_set_style_text_font(result_icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(result_icon_lbl, LV_ALIGN_CENTER, 0, -font_px);

    /* 结果消息 */
    lv_obj_t *result_lbl = lv_label_create(content);
    lv_label_set_text(result_lbl, result_msg ? result_msg : "");
    lv_obj_set_style_text_color(result_lbl, lv_color_hex(result_color), 0);
    lv_obj_set_style_text_font(result_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_set_style_text_align(result_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(result_lbl, LV_ALIGN_CENTER, 0, font_px + 4);

    /* 操作提示 */
    lv_obj_t *hint_lbl = lv_label_create(content);
    lv_label_set_text(hint_lbl, "B:返回");
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_style_text_font(hint_lbl, lv_font_cn_get(font_px), 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    ui_dock_create(scr, 1, 0);
}

static void python_app_destroy(void)
{
    ESP_LOGI(TAG, "Python app destroy");
    /* 停止 Python 脚本任务（协作式） */
    python_app_stop_running();
}

static bool python_app_on_key(int key)
{
    /* Python 游戏运行时：将按键转发到 Python 队列（方向控制等） */
    if (s_py_task_running && xiaomiao_button_task_is_active()) {
        xm_btn_event_t evt = { .key = key, .is_long_press = false };
        xiaomiao_button_push(&evt);
        /* B 键仍需退出应用，其余按键全部交给 Python */
        if (key != KEY_B) {
            return true;
        }
    }

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
 *   ├── app.json     # 应用元数据（名称、图标、颜色、签名等）
 *   └── main.py      # 入口文件
 *
 * app.json 格式：
 * {
 *   "id": "com.xiaomiao.appname",
 *   "name": "应用名",
 *   "icon": "图标字符（1-2个LVGL符号）",
 *   "color": "图标颜色（十六进制，如"#FF0000"）",
 *   "version": "1.0",
 *   "author": "作者名",
 *   "signature": "A1B2C3D4"
 * }
 *
 * 如果 app.json 不存在，则使用目录名作为应用名，默认图标和颜色。
 * 如果没有 signature 字段，应用将被阻止安装。
 */

/* 简单哈希函数：FNV-1a 算法，用于签名验证 */
static uint32_t app_simple_hash(const char *data, int len)
{
    uint32_t hash = 0x811C9DC5; // FNV-1a 初始值
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 0x01000193; // FNV-1a 素数
    }
    return hash;
}

/* 解析 app.json 中的字符串值（前置声明，供 app_verify_signature 使用） */
static char* json_get_string(const char *json, const char *key, char *buf, int buf_size);

/*
 * 验证应用签名。
 * 
 * 签名机制：
 * 1. 从 app.json 中提取 name + icon + color + version + author 拼接成字符串
 * 2. 使用 FNV-1a 哈希计算校验和
 * 3. 将校验和与 app.json 中的 signature 字段比较
 * 
 * signature 计算方式（Python脚本，供开发者使用）：
 *   data = f"{name}|{icon}|{color}|{version}|{author}"
 *   hash = 0x811C9DC5
 *   for c in data.encode():
 *       hash ^= c
 *       hash = (hash * 0x01000193) & 0xFFFFFFFF
 *   sig = format(hash, '08X')
 */
static bool app_verify_signature(const char *json_content)
{
    if (!json_content) return false;
    
    // 提取签名字段
    char signature[16];
    if (!json_get_string(json_content, "signature", signature, sizeof(signature))) {
        ESP_LOGW(TAG, "  No signature found in app.json");
        return false;
    }
    
    // 提取用于签名的字段
    char name_buf[64], icon_buf[16], color_buf[16], version_buf[16], author_buf[64];
    bool has_name = json_get_string(json_content, "name", name_buf, sizeof(name_buf)) != NULL;
    bool has_icon = json_get_string(json_content, "icon", icon_buf, sizeof(icon_buf)) != NULL;
    bool has_color = json_get_string(json_content, "color", color_buf, sizeof(color_buf)) != NULL;
    bool has_version = json_get_string(json_content, "version", version_buf, sizeof(version_buf)) != NULL;
    bool has_author = json_get_string(json_content, "author", author_buf, sizeof(author_buf)) != NULL;
    
    // 构建签名字符串：name|icon|color|version|author
    char sig_data[256];
    snprintf(sig_data, sizeof(sig_data), "%s|%s|%s|%s|%s",
             has_name ? name_buf : "",
             has_icon ? icon_buf : "",
             has_color ? color_buf : "",
             has_version ? version_buf : "",
             has_author ? author_buf : "");
    
    // 计算哈希
    uint32_t hash = app_simple_hash(sig_data, strlen(sig_data));
    
    // 将哈希转换为十六进制字符串
    char hash_str[16];
    snprintf(hash_str, sizeof(hash_str), "%08X", (unsigned int)hash);
    
    // 比较签名（不区分大小写）
    bool valid = (strcasecmp(signature, hash_str) == 0);
    
    if (!valid) {
        ESP_LOGW(TAG, "  Signature mismatch: expected=%s, got=%s", hash_str, signature);
    }
    
    return valid;
}

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
    int blocked_count = 0;
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
        
        // 读取 app.json（可选，但签名验证需要）
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
        
        // 读取应用ID（用于签名验证）
        char app_id_buf[64];
        if (json_content && json_get_string(json_content, "id", app_id_buf, sizeof(app_id_buf))) {
            app->app_id = strdup(app_id_buf);
        }
        
        // ========== 应用安装阻止检查 ==========
        // 验证签名：如果 app.json 存在且有 signature 字段，验证签名
        bool signature_valid = false;
        char signature_buf[16];
        if (json_content && json_get_string(json_content, "signature", signature_buf, sizeof(signature_buf))) {
            signature_valid = app_verify_signature(json_content);
        }
        
        // 检查安装权限
        app_install_status_t install_status = app_check_install_permission(
            app->app_id, 
            signature_valid ? signature_buf : NULL
        );
        
        if (install_status != APP_INSTALL_OK) {
            // 应用被阻止：记录日志，但仍在列表中标记为阻止状态
            // 这样用户可以在应用管理中看到被阻止的应用
            ESP_LOGW(TAG, "  [%d] %s -> BLOCKED (%s)", 
                     count, app->name, app_install_status_desc(install_status));
            app->install_status = install_status;
            blocked_count++;
        } else {
            app->install_status = APP_INSTALL_OK;
            ESP_LOGI(TAG, "  [%d] %s -> %s (verified)", count, app->name, main_path);
        }
        
        // 入口文件路径
        app->py_entry = strdup(main_path);
        
        if (json_content) free(json_content);
        
        count++;
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Found %d MicroPython apps (%d blocked)", count, blocked_count);
    return count;
}

#pragma GCC diagnostic pop