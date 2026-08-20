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
#include "mpthreadport.h"
#include "poincare/mp_xiaomiao.h"  /* xiaomiao 扩展模块：framebuffer 初始化 */

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

/* MicroPython 运行时初始化所需的最小任务栈（用于 mp_thread_init 的 GC 栈扫描） */
#define POINCARE_MP_THREAD_STACK_SIZE (64 * 1024)

/* ========== NLR jump fail 处理 ========== */
void nlr_jump_fail(void *val)
{
    /* 不再重启系统，改为记录错误日志并返回。
     * 上层（app_micropython.c 的 python_app_activate）会捕获执行失败
     * 并显示错误信息，用户可按 B 键安全退出。 */
    ESP_LOGE(TAG, "NLR jump failed, val=%p - MicroPython runtime error, returning to caller", val);
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

    /* 初始化 MicroPython 核心 */
    /*
     * 关键：必须先初始化线程状态（mp_thread_init），否则 MP_STATE_THREAD(x) 宏
     * 会展开为 mp_thread_get_state()->x，而 mp_thread_get_state() 通过 FreeRTOS
     * 任务本地存储指针（TLS, index=1）获取线程状态。若未初始化，该指针为 NULL，
     * 对 NULL 解引用写入 stack_top 会导致 StoreProhibited 崩溃（EXCVADDR=0x00000000）。
     *
     * 官方 ESP32 端口在 main.c 中也是先调用 mp_thread_init() 再初始化运行时。
     * 崩溃 PC 0x400ea49e 正位于 mp_stack_ctrl_init()（stackctrl.c）内，与上述一致。
     *
     * 注意：仅当 MICROPY_PY_THREAD=1 时需要（mp_thread_init 受该宏保护）。
     * 若线程被禁用，MP_STATE_THREAD 直接访问 mp_state_ctx.thread，无需此步骤。
     */
    #if MICROPY_PY_THREAD
    mp_thread_init(pxTaskGetStackStart(NULL), POINCARE_MP_THREAD_STACK_SIZE / sizeof(uintptr_t));
    #endif
    mp_stack_ctrl_init();
    gc_init(s_heap, (void *)((uint32_t)s_heap + heap_size));
    mp_init();
    readline_init0();

    /* 暂时禁用 ESP32 端口外设初始化（避免与已有驱动冲突） */
    // machine_init();
    // machine_pins_init();

    /* 添加系统路径 */
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));

    /* 初始化 xiaomiao 扩展模块 framebuffer（40KB PSRAM，供 Python 脚本绘制） */
    if (!xiaomiao_display_init()) {
        ESP_LOGW(TAG, "Failed to init xiaomiao framebuffer (PSRAM)");
    } else {
        ESP_LOGI(TAG, "xiaomiao framebuffer ready (%d x %d RGB565)", XM_SCREEN_W, XM_SCREEN_H);
    }

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

/* 确保当前任务的 MicroPython 线程状态（TLS 指针）已设置。
 * 关键：mp_thread_init() 只在初始化时调用一次（通常位于 ui_init_task 中），
 * 它通过 vTaskSetThreadLocalStoragePointer(NULL, 1, &mp_state_ctx.thread) 设置
 * 当前任务的 TLS 指针。但按键处理、应用 activate 等运行在 main 任务中，
 * 其 TLS 指针从未被设置，导致 mp_thread_get_state() 返回 NULL。
 * 此时 MP_STATE_THREAD(x) 展开为 mp_thread_get_state()->x，对 NULL 解引用
 * 会在 gc_alloc 等函数中触发 LoadProhibited 崩溃（EXCVADDR=0x00000008）。
 * 因此在每次执行脚本前，确保当前任务已绑定到全局线程状态。 */
static void poincare_ensure_thread_state(void)
{
#if MICROPY_PY_THREAD
    if (mp_thread_get_state() == NULL) {
        mp_thread_set_state(&mp_state_ctx.thread);
    }
#endif
}

int poincare_runtime_exec(const char *source, const char *source_name)
{
    if (!source) {
        ESP_LOGE(TAG, "NULL source");
        return -1;
    }

    poincare_ensure_thread_state();

    /* 重置 C 栈顶到当前任务栈（py_run_task / main 任务），
     * 避免沿用 ui_init_task 初始化时记录的栈顶。
     * 若 stack_top 指向其他任务的栈，mp_cstack_check() 的栈用量
     * 计算会错误，且深调用链下可能破坏 nlr_buf 导致 NLR jump failed。 */
    mp_stack_ctrl_init();

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

    poincare_ensure_thread_state();

    /* 重置 C 栈顶到当前任务栈（见 poincare_runtime_exec 注释） */
    mp_stack_ctrl_init();

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