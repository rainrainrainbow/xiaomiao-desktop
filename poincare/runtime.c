/**
 * @file poincare/runtime.c
 * @brief Poincaré - Script Engine: Runtime Implementation
 *
 * MicroPython 运行时集成实现。
 * 提供脚本引擎初始化和执行接口，基于 v64 的 MicroPython 集成方案。
 * 使用 PSRAM 作为 GC 堆（64KB），支持异常捕获。
 */

#include "poincare/runtime.h"
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

static const char *TAG = "POINCARE";

/* ========== 内部状态 ========== */
static bool s_initialized = false;
static void *s_heap = NULL;
static size_t s_heap_size = 0;

/* 默认 GC 堆大小（64KB PSRAM） */
#define POINCARE_DEFAULT_HEAP_SIZE (64 * 1024)

/* ========== NLR jump fail 处理 ========== */
void nlr_jump_fail(void *val)
{
    ESP_LOGE(TAG, "NLR jump failed, val=%p", val);
    esp_restart();
}

/* ========== Native code commit stub ========== */
__attribute__((weak)) void *esp_native_code_commit(void *buf, size_t len, void *reloc)
{
    ESP_LOGW(TAG, "esp_native_code_commit called (should not happen with EMIT_NATIVE=0)");
    return NULL;
}

/* ========== 初始化/销毁 ========== */

bool poincare_runtime_init(size_t heap_size)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Runtime already initialized");
        return true;
    }

    if (heap_size == 0) {
        heap_size = POINCARE_DEFAULT_HEAP_SIZE;
    }
    s_heap_size = heap_size;

    ESP_LOGI(TAG, "Initializing MicroPython runtime (heap=%d bytes)", heap_size);

    /* 在 PSRAM 分配 MicroPython GC 堆 */
    s_heap = heap_caps_malloc(heap_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_heap == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MicroPython heap from PSRAM (%d bytes)", heap_size);
        /* 回退到 DRAM */
        s_heap = malloc(heap_size);
        if (s_heap == NULL) {
            ESP_LOGE(TAG, "Failed to allocate MicroPython heap from DRAM either");
            return false;
        }
        ESP_LOGW(TAG, "MicroPython heap allocated from DRAM (not PSRAM)");
    }

    /* 初始化 MicroPython 运行时 */
    mp_stack_ctrl_init();
    gc_init(s_heap, (void *)((uint32_t)s_heap + heap_size));
    mp_init();
    readline_init0();

    /* 初始化 ESP32 端口外设 */
    machine_init();
    machine_pins_init();

    /* 添加系统路径 */
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));

    s_initialized = true;
    ESP_LOGI(TAG, "MicroPython runtime initialized successfully");
    return true;
}

void poincare_runtime_deinit(void)
{
    if (!s_initialized) return;

    ESP_LOGI(TAG, "Deinitializing MicroPython runtime");
    mp_deinit();

    if (s_heap) {
        free(s_heap);
        s_heap = NULL;
    }

    s_initialized = false;
    s_heap_size = 0;
    ESP_LOGI(TAG, "MicroPython runtime deinitialized");
}

bool poincare_runtime_is_ready(void)
{
    return s_initialized;
}

/* ========== 脚本执行 ========== */

int poincare_runtime_exec(const char *source, const char *source_name)
{
    if (!source) {
        ESP_LOGE(TAG, "NULL source");
        return -1;
    }

    if (!poincare_runtime_init(s_heap_size)) {
        ESP_LOGE(TAG, "Runtime not initialized");
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

int poincare_runtime_exec_file(const char *filename)
{
    if (!filename) {
        ESP_LOGE(TAG, "NULL filename");
        return -1;
    }

    if (!poincare_runtime_init(s_heap_size)) {
        ESP_LOGE(TAG, "Runtime not initialized");
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

/* ========== 模块注册 ========== */

bool poincare_runtime_register_module(const char *module_name, void *init_func)
{
    if (!module_name || !init_func) return false;

    if (!poincare_runtime_init(s_heap_size)) {
        return false;
    }

    /* 注意：MicroPython v1.28.0 使用 MP_REGISTER_MODULE 宏在编译时注册模块，
     * 运行时注册需要通过 mp_builtin_extensible_module_map 实现。
     * 当前简化实现：直接返回 true（模块须在编译时通过 MP_REGISTER_MODULE 注册） */
    ESP_LOGW(TAG, "Module registration at runtime is not supported in MicroPython v1.28.0");
    ESP_LOGW(TAG, "Please use MP_REGISTER_MODULE macro at compile time for module: %s", module_name);
    return true;
}