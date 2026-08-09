/**
 * @file app_micropython.c
 * @brief MicroPython应用支持 - 加载和运行SD卡上的Python应用
 */

#include "app_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_PY";

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
    
    ESP_LOGI(TAG, "Python app init: %s (entry=%s)", app->name, app->py_entry);
    
    // TODO: 初始化MicroPython运行时
    // TODO: 加载并执行app->py_entry
}

static void python_app_activate(void)
{
    ESP_LOGI(TAG, "Python app activate");
    // TODO: 恢复Python应用状态
}

static void python_app_destroy(void)
{
    ESP_LOGI(TAG, "Python app destroy");
    // TODO: 清理Python运行时资源
}

static bool python_app_on_key(int key)
{
    // TODO: 将按键事件传递给Python应用
    if (key == KEY_B) {
        ui_stack_pop();
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
int app_micropython_scan(const char *base_path, app_def_t *apps, int max_count)
{
    // TODO: 实现SD卡扫描逻辑
    // 1. 打开base_path目录
    // 2. 遍历子目录
    // 3. 检查是否存在app.json和main.py
    // 4. 解析app.json获取应用信息
    // 5. 填充apps数组
    
    ESP_LOGI(TAG, "Scanning %s for Python apps...", base_path);
    
    // 暂时返回0
    return 0;
}