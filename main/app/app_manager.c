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

/* ========== 最近任务列表 ========== */
static const app_def_t *s_recents[MAX_RECENTS];
static int s_recents_count = 0;

/* ========== 应用管理器初始化 ========== */
void app_manager_init(void)
{
    s_builtin_count = 0;
    s_python_count = 0;
    s_recents_count = 0;
    memset(s_builtin_apps, 0, sizeof(s_builtin_apps));
    memset(s_python_apps, 0, sizeof(s_python_apps));
    memset(s_recents, 0, sizeof(s_recents));
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

    // 记录到最近任务
    app_manager_add_recents(app);

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

/* ========== 最近任务 ========== */
void app_manager_add_recents(const app_def_t *app)
{
    if (!app) return;

    // 去重：如果已在列表中，移到最前
    for (int i = 0; i < s_recents_count; i++) {
        if (s_recents[i] == app) {
            // 移到最前
            for (int j = i; j > 0; j--) {
                s_recents[j] = s_recents[j - 1];
            }
            s_recents[0] = app;
            return;
        }
    }

    // 新增：插入到最前
    if (s_recents_count < MAX_RECENTS) {
        for (int j = s_recents_count; j > 0; j--) {
            s_recents[j] = s_recents[j - 1];
        }
        s_recents[0] = app;
        s_recents_count++;
    } else {
        // 列表满：丢掉最后一个，前移后插入最前
        for (int j = MAX_RECENTS - 1; j > 0; j--) {
            s_recents[j] = s_recents[j - 1];
        }
        s_recents[0] = app;
    }
}

const app_def_t* app_manager_get_recents(int *count)
{
    if (count) *count = s_recents_count;
    // 返回最近任务对应的应用定义数组
    // 注意：s_recents 是指针数组，这里返回的是指向数组的指针。
    // 我们改为返回内置应用定义数组，调用方用索引访问。
    // 实际上这里返回 NULL 时 count=0，调用方应通过 index 从 recents 获取。
    if (s_recents_count > 0) {
        return (const app_def_t*)s_recents[0];
    }
    return NULL;
}

/* 获取最近任务的第 i 个应用指针 */
const app_def_t* app_manager_get_recents_at(int i)
{
    if (i >= 0 && i < s_recents_count) {
        return s_recents[i];
    }
    return NULL;
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