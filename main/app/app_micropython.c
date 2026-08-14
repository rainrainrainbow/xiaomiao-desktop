/**
 * @file app_micropython.c
 * @brief MicroPython 运行时集成 - v64 完整 MicroPython 运行时
 * 
 * 提供 MicroPython 运行时初始化和脚本执行引擎。
 * 桌面系统通过此模块执行 Python 脚本，支持异常捕获。
 */

#include "app_manager.h"
#include "app_micropython.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

/* MicroPython 核心头文件 */
#include "py/runtime.h"
#include "py/compile.h"
#include "py/lexer.h"
#include "py/gc.h"
#include "py/nlr.h"
#include "py/mphal.h"
#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objmodule.h"
#include "py/objlist.h"
#include "py/reader.h"
#include "py/qstr.h"
#include "py/stackctrl.h"
#include "py/cstack.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"
#include "modmachine.h"
#include "modesp32.h"

/* modmachine.h 中声明的函数（modmachine.c 作为 INCLUDEFILE 被 extmod/machine.c 包含，
   此处显式声明以确保 main 组件能正确链接） */
void machine_init(void);
void machine_pins_init(void);

static const char *TAG = "APP_PY";

/* ========== MicroPython 运行时状态 ========== */
static bool s_mp_initialized = false;
static void *s_mp_heap = NULL;
#define MP_HEAP_SIZE (64 * 1024)  /* 64KB PSRAM 堆 */

/* ========== MicroPython 运行时初始化 ========== */
bool app_micropython_init(void)
{
    if (s_mp_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing MicroPython runtime...");

    /* 在 PSRAM 分配 MicroPython GC 堆 */
    s_mp_heap = heap_caps_malloc(MP_HEAP_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mp_heap == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MicroPython heap (%d bytes)", MP_HEAP_SIZE);
        return false;
    }

    /* 初始化 MicroPython 运行时 */
    mp_stack_ctrl_init();
    gc_init(s_mp_heap, (void *)((uint32_t)s_mp_heap + MP_HEAP_SIZE));
    mp_init();
    readline_init0();

    /* 初始化 ESP32 端口外设 */
    machine_init();
    machine_pins_init();

    /* 添加系统路径 */
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));

    s_mp_initialized = true;
    ESP_LOGI(TAG, "MicroPython runtime initialized");
    return true;
}

/* ========== 脚本执行引擎 ========== */

/**
 * @brief 执行一段 Python 源码字符串
 * @param source Python 源码
 * @param source_name 源码名称（用于错误报告）
 * @return 0 成功，-1 失败
 */
int app_micropython_exec(const char *source, const char *source_name)
{
    if (!source) {
        ESP_LOGE(TAG, "NULL source");
        return -1;
    }

    if (!app_micropython_init()) {
        ESP_LOGE(TAG, "MicroPython not initialized");
        return -1;
    }

    /* 创建 lexer */
    mp_lexer_t *lex = mp_lexer_new_from_str_len(
        qstr_from_str(source_name ? source_name : "<string>"),
        source, strlen(source), 0);
    if (lex == NULL) {
        ESP_LOGE(TAG, "Failed to create lexer");
        return -1;
    }

    /* 异常捕获 */
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        /* 编译并执行 */
        mp_parse_compile_execute(lex, MP_PARSE_FILE_INPUT, NULL, NULL);
        nlr_pop();
        return 0;
    } else {
        /* 捕获到异常 */
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        if (mp_obj_is_exception_instance(exc)) {
            mp_obj_print_exception(&mp_plat_print, exc);
        }
        ESP_LOGW(TAG, "Python exception in %s", source_name ? source_name : "<string>");
        return -1;
    }
}

/**
 * @brief 执行一个 Python 文件
 * @param filename 文件路径
 * @return 0 成功，-1 失败
 */
int app_micropython_exec_file(const char *filename)
{
    if (!filename) {
        ESP_LOGE(TAG, "NULL filename");
        return -1;
    }

    if (!app_micropython_init()) {
        ESP_LOGE(TAG, "MicroPython not initialized");
        return -1;
    }

    /* 使用 pyexec 执行文件 */
    int ret = pyexec_file(filename);
    if (ret & PYEXEC_FORCED_EXIT) {
        ESP_LOGW(TAG, "Forced exit while executing %s", filename);
        return -1;
    }
    return (ret == 0) ? 0 : -1;
}

/**
 * @brief 检查 MicroPython 是否已初始化
 */
bool app_micropython_is_ready(void)
{
    return s_mp_initialized;
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
    /* 初始化 MicroPython 运行时 */
    app_micropython_init();
}

static void python_app_activate(void)
{
    ESP_LOGI(TAG, "Python app activate");
    /* 执行测试脚本 */
    app_micropython_exec("print('Hello from XiaoMiao MicroPython!')\n", "<boot>");
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
    ESP_LOGW(TAG, "MicroPython scan not implemented yet");
    return 0;
}