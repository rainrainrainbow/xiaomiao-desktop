/**
 * @file app_micropython.c
 * @brief MicroPython应用支持 - v63 完整 MicroPython 运行时集成
 */

#include "app_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

// MicroPython 头文件
#include "py/mpstate.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "extmod/vfs_fat.h"

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

/* ========== MicroPython 运行时状态 ========== */
static bool s_mpy_initialized = false;
static uint8_t *s_mpy_heap = NULL;
static const size_t MPY_HEAP_SIZE = 64 * 1024;  // 64KB PSRAM 堆

/* ========== 初始化 MicroPython 运行时 ========== */
static void mpy_runtime_init(void)
{
    if (s_mpy_initialized) {
        return;
    }
    
    ESP_LOGI(TAG, "Initializing MicroPython runtime...");
    
    // 从 PSRAM 分配堆内存
    s_mpy_heap = heap_caps_malloc(MPY_HEAP_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_mpy_heap) {
        ESP_LOGE(TAG, "Failed to allocate MicroPython heap from PSRAM");
        return;
    }
    
    // 初始化垃圾回收堆
    gc_init(s_mpy_heap, s_mpy_heap + MPY_HEAP_SIZE);
    
    // 初始化 MicroPython 运行时
    mp_init();
    
    // 挂载 FAT 文件系统到 MicroPython VFS（如果 SD 卡已挂载）
    #if MICROPY_VFS_FAT
    // TODO: 在 SD 卡挂载后调用 mp_vfs_fat_mount("/sdcard")
    ESP_LOGI(TAG, "MicroPython VFS ready for FAT filesystem");
    #endif
    
    s_mpy_initialized = true;
    ESP_LOGI(TAG, "MicroPython runtime initialized (heap=%d KB)", MPY_HEAP_SIZE / 1024);
}

/* ========== 执行 Python 脚本 ========== */
static void mpy_execute_script(const char *filepath)
{
    if (!s_mpy_initialized) {
        ESP_LOGE(TAG, "MicroPython runtime not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Executing script: %s", filepath);
    
    // 读取脚本文件
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open script: %s", filepath);
        return;
    }
    
    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // 分配缓冲区
    char *script = malloc(size + 1);
    if (!script) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate script buffer");
        return;
    }
    
    fread(script, 1, size, f);
    script[size] = '\0';
    fclose(f);
    
    // 执行脚本
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_compile_and_import(script, filepath);
        nlr_pop();
        ESP_LOGI(TAG, "Script executed successfully");
    } else {
        // 捕获异常
        mp_obj_t exc = nlr.ret_val;
        mp_obj_print_exception(&mp_plat_print, exc);
        ESP_LOGE(TAG, "Python script execution failed");
    }
    
    free(script);
}

/* ========== MicroPython应用初始化 ========== */
static void python_app_init(void *data)
{
    const app_def_t *app = (const app_def_t *)data;
    if (!app) {
        ESP_LOGE(TAG, "NULL app data");
        return;
    }
    
    ESP_LOGI(TAG, "Python app init: %s (entry=%s)", app->name, app->py_entry);
    
    // 初始化 MicroPython 运行时（如果尚未初始化）
    mpy_runtime_init();
    
    // 执行 Python 脚本
    if (s_mpy_initialized && strlen(app->py_entry) > 0) {
        mpy_execute_script(app->py_entry);
    }
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
    ESP_LOGW(TAG, "MicroPython scan not implemented yet (v63+)");
    return 0;
}