/**
 * @file app_manager.c
 * @brief 应用管理器实现
 */

#include "app_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_MGR";

/* ========== 内置应用列表 ========== */
#define MAX_BUILTIN_APPS 16
static app_def_t s_builtin_apps[MAX_BUILTIN_APPS];
static int s_builtin_count = 0;

/* ========== MicroPython应用列表 ========== */
#define MAX_PYTHON_APPS 16
static app_def_t s_python_apps[MAX_PYTHON_APPS];
static int s_python_count = 0;

/* ========== 应用管理器初始化 ========== */
void app_manager_init(void)
{
    s_builtin_count = 0;
    s_python_count = 0;
    memset(s_builtin_apps, 0, sizeof(s_builtin_apps));
    memset(s_python_apps, 0, sizeof(s_python_apps));
    ESP_LOGI(TAG, "App manager initialized");
}

/* ========== 注册内置应用 ========== */
void app_register_builtin(const app_def_t *app)
{
    if (s_builtin_count >= MAX_BUILTIN_APPS) {
        ESP_LOGW(TAG, "Builtin app limit reached");
        return;
    }
    s_builtin_apps[s_builtin_count++] = *app;
    ESP_LOGI(TAG, "Registered builtin app: %s", app->name);
}

/* ========== 获取内置应用列表 ========== */
const app_def_t* app_manager_get_builtin(int *count)
{
    if (count) *count = s_builtin_count;
    return s_builtin_apps;
}

/* ========== 获取MicroPython应用列表 ========== */
const app_def_t* app_manager_get_micropython(int *count)
{
    if (count) *count = s_python_count;
    return s_python_apps;
}

/* ========== 扫描SD卡应用 ========== */
int app_manager_scan_sdcard(void)
{
    // TODO: 实现SD卡扫描逻辑
    // 扫描 /sdcard/apps/ 目录下的应用
    ESP_LOGI(TAG, "Scanning SD card for MicroPython apps...");
    
    // 暂时返回0，后续实现
    return 0;
}

/* ========== 启动应用 ========== */
void app_manager_launch(const app_def_t *app)
{
    if (!app) {
        ESP_LOGE(TAG, "NULL app pointer");
        return;
    }

    ESP_LOGI(TAG, "Launching app: %s (type=%d)", app->name, app->type);

    if (app->type == APP_TYPE_BUILTIN) {
        // 内置应用：根据名称查找页面回调并推入页面栈
        const page_callbacks_t *cbs = app_builtin_get_callbacks(app->name);
        if (cbs) {
            ui_stack_push(PAGE_APP_PLACEHOLDER, cbs, NULL);
            ESP_LOGI(TAG, "Pushed builtin app: %s", app->name);
        } else {
            ESP_LOGE(TAG, "No callbacks for builtin app: %s", app->name);
        }
        // 兼容旧的launch_cb接口
        if (app->launch_cb) {
            app->launch_cb();
        }
    } else if (app->type == APP_TYPE_MICROPYTHON) {
        // TODO: 启动MicroPython应用
        ESP_LOGW(TAG, "MicroPython app launch not implemented yet");
    }
}

/* ========== 获取应用页面回调 ========== */
page_callbacks_t app_manager_get_callbacks(const app_def_t *app)
{
    page_callbacks_t callbacks = {0};
    
    if (!app) {
        return callbacks;
    }
    
    // 根据应用类型返回不同的回调
    if (app->type == APP_TYPE_BUILTIN) {
        // 内置应用使用统一的占位页面
        // 具体实现在app_builtin.c中
    } else if (app->type == APP_TYPE_MICROPYTHON) {
        // MicroPython应用使用Python运行时页面
        // 具体实现在app_micropython.c中
    }
    
    return callbacks;
}