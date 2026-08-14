/**
 * @file app_micropython.c
 * @brief MicroPython应用支持 - 加载和运行SD卡上的Python应用
 */

#include "app_manager.h"
#include "system/sys_sdcard.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

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
    // v62: 仅注册应用，v63+ 将集成完整 MicroPython 运行时
}

static void python_app_activate(void)
{
    ESP_LOGI(TAG, "Python app activate");
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
int app_micropython_scan(const char *base_path, app_def_t *apps, int max_count)
{
    ESP_LOGI(TAG, "Scanning %s for Python apps...", base_path);
    
    // 初始化 SD 卡（如果尚未挂载）
    if (!sys_sdcard_is_mounted()) {
        int ret = sys_sdcard_init();
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to mount SD card: %d", ret);
            return 0;
        }
    }
    
    // 扫描应用
    app_meta_t metas[SDCARD_MAX_APPS];
    int count = sys_sdcard_scan_apps(metas, SDCARD_MAX_APPS);
    
    // 转换为 app_def_t 格式
    int registered = 0;
    for (int i = 0; i < count && registered < max_count; i++) {
        app_def_t *app = &apps[registered];
        
        strncpy(app->name, metas[i].name, sizeof(app->name) - 1);
        app->name[sizeof(app->name) - 1] = '\0';
        
        strncpy(app->icon_glyph, metas[i].icon, sizeof(app->icon_glyph) - 1);
        app->icon_glyph[sizeof(app->icon_glyph) - 1] = '\0';
        
        strncpy(app->py_entry, metas[i].entry, sizeof(app->py_entry) - 1);
        app->py_entry[sizeof(app->py_entry) - 1] = '\0';
        
        app->type = APP_TYPE_MICROPYTHON;
        app->callbacks = app_micropython_get_callbacks();
        
        ESP_LOGI(TAG, "Registered Python app: %s (%s)", app->name, app->py_entry);
        registered++;
    }
    
    return registered;
}