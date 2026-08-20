/**
 * @file app_manager.c
 * @brief 应用管理器实现
 */

#include "app_manager.h"
#include "app_builtin.h"
#include "app_micropython.h"
#include "bg_manager.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_MGR";

/* ========== 内置应用列表 ========== */
static app_def_t s_builtin_apps[MAX_BUILTIN_APPS];
static int s_builtin_count = 0;

/* ========== MicroPython应用列表 ========== */
static app_def_t s_python_apps[MAX_PYTHON_APPS];
static int s_python_count = 0;

/* ========== 最近任务列表 ========== */
static const app_def_t *s_recents[MAX_RECENTS];
static int s_recents_count = 0;

/* ========== 当前运行的应用 ========== */
static const char *s_current_app_name = NULL;

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
    bg_manager_init();
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
    // 扫描 /sdcard/apps/ 目录下的 MicroPython 应用
    ESP_LOGI(TAG, "Scanning SD card for MicroPython apps...");
    
    int count = app_micropython_scan("/sdcard/apps", s_python_apps, MAX_PYTHON_APPS);
    s_python_count = count;
    
    ESP_LOGI(TAG, "SD card scan complete: found %d MicroPython apps", count);
    return count;
}

/* ========== 启动应用 ========== */
void app_manager_launch(const app_def_t *app)
{
    if (!app) {
        ESP_LOGE(TAG, "NULL app pointer");
        return;
    }

    ESP_LOGI(TAG, "Launching app: %s (type=%d)", app->name, app->type);

    // 记录当前应用名（用于状态栏显示）
    s_current_app_name = app->name;

    // 更新状态栏左上角为当前应用名（使用本地化显示名）
    ui_statusbar_set_title(app_builtin_get_display_name(app->name));

    // 记录到后台管理器（标记为前台运行）
    bg_manager_on_launch(app->name);

    // 记录到最近任务
    app_manager_add_recents(app);

    if (app->type == APP_TYPE_BUILTIN) {
        // 特殊处理："应用"图标 → 进入设置中的应用管理二级页面
        if (strcmp(app->name, "应用") == 0) {
            app_launch_app_manager();
            ESP_LOGI(TAG, "Launched app manager via settings");
        } else {
            // 其他内置应用：根据名称查找页面回调并推入页面栈
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
        }
    } else if (app->type == APP_TYPE_MICROPYTHON) {
        // 启动MicroPython应用：推入Python页面，传入app数据
        const page_callbacks_t *py_cbs = app_micropython_get_callbacks();
        if (py_cbs) {
            ui_stack_push(PAGE_APP_PLACEHOLDER, py_cbs, (void*)app);
            ESP_LOGI(TAG, "Pushed MicroPython app: %s", app->name);
        } else {
            ESP_LOGE(TAG, "No callbacks for MicroPython app: %s", app->name);
        }
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
    // s_recents 是指针数组（const app_def_t*[]），不是连续的结构体数组，
    // 不能直接返回指针数组的第一个元素强转为结构体指针。
    // 调用方应通过 app_manager_get_recents_at(i) 按索引获取条目。
    // 返回 NULL 表示调用方应通过 count 知晓数量，通过 _at 函数获取具体条目。
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

/* ========== 获取当前应用名 ========== */
const char* app_manager_get_current_name(void)
{
    return s_current_app_name;
}

void app_manager_clear_current(void)
{
    s_current_app_name = NULL;
}

/* ========== 应用安装阻止机制 ========== */

/*
 * 内置可信应用白名单。
 * 这些应用ID是经过官方签名的可信应用，允许从SD卡安装。
 * 格式：{"应用ID"}
 * 空列表表示只允许签名应用，不允许任何未签名应用。
 */
static const char *s_trusted_app_ids[] = {
    // 官方应用示例（实际使用时替换为真实应用ID）
    // "com.xiaomiao.clock",
    // "com.xiaomiao.weather",
    // "com.xiaomiao.calculator",
};
#define TRUSTED_APP_COUNT (sizeof(s_trusted_app_ids) / sizeof(s_trusted_app_ids[0]))

/* 检查应用ID是否在白名单中 */
static bool app_is_in_whitelist(const char *app_id)
{
    if (!app_id || !app_id[0]) return false;
    
    for (int i = 0; i < (int)TRUSTED_APP_COUNT; i++) {
        if (strcmp(app_id, s_trusted_app_ids[i]) == 0) {
            return true;
        }
    }
    return false;
}

app_install_status_t app_check_install_permission(const char *app_id, const char *signature)
{
    // 模式1：白名单模式 — 如果应用ID在白名单中，直接允许
    if (app_id && app_id[0] && app_is_in_whitelist(app_id)) {
        return APP_INSTALL_OK;
    }
    
    // 模式2：签名模式 — 如果有签名，允许安装
    // （签名内容已在 app_micropython_scan 中验证）
    if (signature && signature[0]) {
        return APP_INSTALL_OK;
    }
    
    // 模式3：无签名且不在白名单 — 阻止安装
    ESP_LOGW(TAG, "App install blocked: id=%s, no signature and not in whitelist", 
             app_id ? app_id : "(null)");
    return APP_INSTALL_BLOCKED;
}

const char* app_install_status_desc(app_install_status_t status)
{
    switch (status) {
        case APP_INSTALL_OK:
            return lang_get(STR_INSTALL_OK);
        case APP_INSTALL_BLOCKED:
            return lang_get(STR_INSTALL_BLOCKED);
        case APP_INSTALL_UNTRUSTED:
            return lang_get(STR_INSTALL_UNTRUSTED);
        default:
            return lang_get(STR_INSTALL_UNKNOWN);
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
        const page_callbacks_t *py_cbs = app_micropython_get_callbacks();
        if (py_cbs) {
            callbacks = *py_cbs;
        }
    }
    
    return callbacks;
}