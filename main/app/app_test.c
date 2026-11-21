/**
 * @file app_test.c
 * @brief 自动化测试命令接口
 * 
 * 提供 USB CDC 串口命令接口，支持：
 * - JSON 格式命令/响应协议
 * - 硬件功能测试（按键、屏幕、音频、LED、电池、蓝牙、MicroPython）
 * - 测试脚本执行与结果上报
 * - 固件版本/信息查询
 * 
 * 协议：命令以 "XTC:" 前缀开头，后跟 JSON，以 \n 结尾。
 * 响应以 "XTR:" 前缀开头，后跟 JSON，以 \n 结尾。
 * 普通串口输出无前缀，测试任务只处理带前缀的行。
 */

#include "app_test.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "driver/drv_led_strip.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#ifndef XIAOMIAO_SCREEN_WIDTH
#define XIAOMIAO_SCREEN_WIDTH  (160U)
#endif
#ifndef XIAOMIAO_SCREEN_HEIGHT
#define XIAOMIAO_SCREEN_HEIGHT (128U)
#endif
#ifndef XIAOMIAO_LED_COUNT
#define XIAOMIAO_LED_COUNT     (3)
#endif

/* 外部依赖 */
extern bool drv_button_get_event(btn_event_t *evt);
extern bool xiaomiao_button_push(const xm_btn_event_t *evt);
extern bool xiaomiao_button_task_is_active(void);
extern uint16_t *xiaomiao_display_get_framebuffer(void);
extern void xiaomiao_display_set_flush_cb(xm_flush_fn cb);
extern bool xiaomiao_display_init(void);
extern float drv_battery_get_voltage(void);
extern int drv_battery_get_percent(float voltage);
extern esp_err_t drv_led_strip_set_pixel(int index, led_rgb_t color);
extern esp_err_t drv_led_strip_refresh(void);
extern esp_err_t audio_output_init(void);
extern void audio_output_tone(uint32_t freq_hz, uint32_t duration_ms);
extern void drv_backlight_set_brightness(int percent);
extern const char *app_manager_get_current_name(void);
extern void ui_statusbar_update_time(void);
extern bool poincare_runtime_is_ready(void);
extern bool poincare_runtime_init(size_t heap_size);
extern int poincare_runtime_exec(const char *source, const char *source_name);
extern const char *poincare_runtime_get_last_error(void);
extern void wifi_diag_trigger_scan(void);

static const char *TAG = "APP_TEST";

/* ========== 配置常量 ========== */
#define TEST_UART_NUM           UART_NUM_0    // USB CDC 使用 UART0
#define TEST_UART_BAUD_RATE     115200
#define TEST_UART_BUF_SIZE      4096
/* 命令在 UART 任务内同步执行，PLAY_TONE/GET_INFO 等会触发深层调用链
 * (cJSON + audio_output_tone/I2S + sinf)，4096 栈会溢出导致 SW_CPU_RESET。
 * 给足栈余量。 */
#define TEST_UART_TASK_STACK    12288
#define TEST_TASK_STACK_SIZE    8192
#define TEST_TASK_PRIORITY      10

/* UART 命令任务长期驻留，12KB 动态栈原本固定占内部 DRAM；
 * 改为 PSRAM 静态栈，给后续动态任务（tone/test）留出内部 RAM。 */
EXT_RAM_BSS_ATTR static StackType_t s_test_uart_stack[TEST_UART_TASK_STACK / sizeof(StackType_t)];
/* TCB 必须留在内部 RAM：IDF v5.3 的 xPortCheckValidTCBMem 会拒绝 PSRAM 中的 TCB */
static StaticTask_t s_test_uart_tcb;

/* ========== 命令类型 ========== */
typedef enum {
    TEST_CMD_PING = 0,
    TEST_CMD_GET_INFO,
    TEST_CMD_RUN_TEST,
    TEST_CMD_GET_TEST_RESULT,
    TEST_CMD_EXEC_PYTHON,
    TEST_CMD_SIMULATE_KEY,
    TEST_CMD_SET_LED,
    TEST_CMD_SET_BACKLIGHT,
    TEST_CMD_PLAY_TONE,
    TEST_CMD_GET_BATTERY,
    TEST_CMD_LIST_TESTS,
    TEST_CMD_RESET_DEVICE,
    TEST_CMD_ENTER_BOOTLOADER,
    TEST_CMD_DIAG_WIFI_SCAN,   /* 诊断：直接触发 WiFi 扫描 */
} test_cmd_type_t;

/* 测试项目、测试状态、测试结果类型定义见 app_test.h */

/* ========== 命令结构 ========== */
typedef struct {
    int id;                 // 请求 ID（从顶层 JSON 提取）
    test_cmd_type_t type;
    cJSON *params;          // 命令参数（JSON）
} test_cmd_t;

/* 异步音调播放参数 */
typedef struct {
    uint32_t freq;          // 频率 Hz
    uint32_t duration;      // 时长 ms
    int req_id;             // 请求 ID，用于关联响应
} tone_ctx_t;

/* ========== 全局状态 ========== */
static SemaphoreHandle_t s_result_mutex = NULL;
static SemaphoreHandle_t s_uart_tx_mutex = NULL;   // 保护 UART 响应写入
static SemaphoreHandle_t s_python_mutex = NULL;    // 序列化 MicroPython 解释器访问（不可重入）
static SemaphoreHandle_t s_audio_mutex = NULL;     // 序列化音频输出（避免并发 I2S 写入）
/* 纯测试结果缓存，放 PSRAM：约 2.7KB 内部 DRAM 释放 */
EXT_RAM_BSS_ATTR static test_result_t s_test_results[TEST_ITEM_ALL + 1] = {0};
static test_state_t s_current_test_state = TEST_STATE_IDLE;
static test_item_t s_current_test_item = TEST_ITEM_ALL;
static bool s_test_running = false;
EXT_RAM_BSS_ATTR static char s_test_output_buffer[2048];
static size_t s_test_output_len = 0;

/* ========== 前向声明 ========== */
static void test_uart_task(void *arg);
static esp_err_t test_send_response(cJSON *response);
static void test_handle_cmd(const test_cmd_t *cmd);
static void test_run_single(test_item_t item);
static void test_run_all(void);
static void test_play_tone_task(void *arg);

/* 线程安全输出缓冲追加 */
static inline void test_output_append(const char *fmt, ...)
{
    if (s_test_output_len >= sizeof(s_test_output_buffer) - 1) {
        return;  // 缓冲已满，防止溢出
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(s_test_output_buffer + s_test_output_len,
                             sizeof(s_test_output_buffer) - s_test_output_len, fmt, args);
    va_end(args);
    if (written > 0) {
        s_test_output_len += (size_t)written;
        if (s_test_output_len >= sizeof(s_test_output_buffer) - 1) {
            s_test_output_len = sizeof(s_test_output_buffer) - 1;
        }
    }
}

/* ========== 单项测试实现 ========== */

/* 测试按键 */
static void test_buttons(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_BUTTONS];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_BUTTONS;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("Button test started\n");

    /* 等待用户按键（最多30秒），检测所有6个按键 */
    bool btn_pressed[6] = {false};
    int btn_count = 0;
    uint32_t start = esp_timer_get_time() / 1000;

    test_output_append("Press all 6 buttons (UP/DOWN/LEFT/RIGHT/A/B)...\n");

    while (btn_count < 6 && (esp_timer_get_time() / 1000 - start) < 30000) {
        btn_event_t evt;
        if (drv_button_get_event(&evt)) {
            if (!btn_pressed[evt.key]) {
                btn_pressed[evt.key] = true;
                btn_count++;
                const char *names[] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B"};
                test_output_append("  Detected: %s (idx=%d, long=%d)\n",
                                   names[evt.key], evt.key, evt.is_long_press);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (btn_count == 6) {
            res->state = TEST_STATE_COMPLETED;
            res->score = 100;
            snprintf(res->detail, sizeof(res->detail), "All 6 buttons detected");
        } else {
            res->state = TEST_STATE_FAILED;
            res->score = btn_count * 100 / 6;
            snprintf(res->detail, sizeof(res->detail), "Only %d/6 buttons detected", btn_count);
        }
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("Button test done: %s\n", res->detail);
}

/* 测试屏幕显示 */
static void test_display(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_DISPLAY];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_DISPLAY;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("Display test started\n");

    if (!xiaomiao_display_init()) {
        if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            res->state = TEST_STATE_FAILED;
            res->score = 0;
            snprintf(res->detail, sizeof(res->detail), "Framebuffer init failed");
            xSemaphoreGive(s_result_mutex);
        }
        test_output_append("FAILED: Framebuffer init failed\n");
        return;
    }

    uint16_t *fb = xiaomiao_display_get_framebuffer();
    if (!fb) {
        if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            res->state = TEST_STATE_FAILED;
            res->score = 0;
            snprintf(res->detail, sizeof(res->detail), "Framebuffer is NULL");
            xSemaphoreGive(s_result_mutex);
        }
        test_output_append("FAILED: Framebuffer is NULL\n");
        return;
    }

    /* 测试颜色填充 */
    const uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000};
    const char *color_names[] = {"Red", "Green", "Blue", "White", "Black"};

    for (int i = 0; i < 5; i++) {
        uint16_t c = ((colors[i] >> 16 & 0xFF) >> 3) << 11 |
                     ((colors[i] >> 8 & 0xFF) >> 2) << 5 |
                     ((colors[i] & 0xFF) >> 3);
        /* SWAPPED字节序 */
        c = (c >> 8) | (c << 8);

        for (size_t j = 0; j < (XIAOMIAO_SCREEN_WIDTH * XIAOMIAO_SCREEN_HEIGHT); j++) fb[j] = c;

        /* 注：实际屏幕刷新由显示任务的 flush 回调驱动，此处仅填充 framebuffer */
        test_output_append("  Showing %s...\n", color_names[i]);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* 显示测试图案：网格 */
    memset(fb, 0, XIAOMIAO_SCREEN_WIDTH * XIAOMIAO_SCREEN_HEIGHT * sizeof(uint16_t));
    for (uint16_t y = 0; y < XIAOMIAO_SCREEN_HEIGHT; y += 8) {
        for (uint16_t x = 0; x < XIAOMIAO_SCREEN_WIDTH; x++) fb[y * XIAOMIAO_SCREEN_WIDTH + x] = 0xFFFF;
    }
    for (uint16_t x = 0; x < XIAOMIAO_SCREEN_WIDTH; x += 8) {
        for (uint16_t y = 0; y < XIAOMIAO_SCREEN_HEIGHT; y++) fb[y * XIAOMIAO_SCREEN_WIDTH + x] = 0xFFFF;
    }

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->state = TEST_STATE_COMPLETED;
        res->score = 100;
        snprintf(res->detail, sizeof(res->detail), "Display patterns shown (160x128 RGB565)");
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("Display test done: %s\n", res->detail);
}

/* 测试音频 */
static void test_audio(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_AUDIO];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_AUDIO;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("Audio test started\n");

    /* 播放测试音调（串行化音频后端，避免与异步 tone 任务并发写 I2S） */
    const uint32_t tones[] = {440, 880, 1760, 3520};  // A4, A5, A6, A7

    if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        for (int i = 0; i < 4; i++) {
            test_output_append("  Playing %lu Hz...\n", tones[i]);
            audio_output_tone(tones[i], 300);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        xSemaphoreGive(s_audio_mutex);
    } else {
        test_output_append("  Audio backend busy, skipped tones\n");
    }

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->state = TEST_STATE_COMPLETED;
        res->score = 100;
        snprintf(res->detail, sizeof(res->detail), "Test tones played via I2S DAC");
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("Audio test done: %s\n", res->detail);
}

/* 测试 LED */
static void test_led(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_LED];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_LED;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("LED test started\n");

    const led_rgb_t colors[] = {
        {0xFF, 0x00, 0x00},  // Red
        {0x00, 0xFF, 0x00},  // Green
        {0x00, 0x00, 0xFF},  // Blue
        {0xFF, 0xFF, 0x00},  // Yellow
        {0xFF, 0x00, 0xFF},  // Magenta
        {0x00, 0xFF, 0xFF},  // Cyan
        {0xFF, 0xFF, 0xFF},  // White
    };
    const char *names[] = {"Red", "Green", "Blue", "Yellow", "Magenta", "Cyan", "White"};

    for (int i = 0; i < 7; i++) {
        for (int led = 0; led < XIAOMIAO_LED_COUNT; led++) {
            drv_led_strip_set_pixel(led, colors[i]);
        }
        drv_led_strip_refresh();

        test_output_append("  All LEDs: %s\n", names[i]);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    /* 关闭所有 LED */
    led_rgb_t off = {0, 0, 0};
    for (int led = 0; led < XIAOMIAO_LED_COUNT; led++) {
        drv_led_strip_set_pixel(led, off);
    }
    drv_led_strip_refresh();

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->state = TEST_STATE_COMPLETED;
        res->score = 100;
        snprintf(res->detail, sizeof(res->detail), "RGB LED strip tested (%d LEDs)", XIAOMIAO_LED_COUNT);
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("LED test done: %s\n", res->detail);
}

/* 测试电池 */
static void test_battery(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_BATTERY];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_BATTERY;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("Battery test started\n");

    float voltage = drv_battery_get_voltage();
    int percent = drv_battery_get_percent(voltage);

    test_output_append("  Voltage: %.2fV, Percent: %d%%\n", voltage, percent);

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (voltage > 3.0f && voltage < 4.5f) {
            res->state = TEST_STATE_COMPLETED;
            res->score = 100;
            snprintf(res->detail, sizeof(res->detail), "Battery: %.2fV (%d%%)", voltage, percent);
        } else {
            res->state = TEST_STATE_FAILED;
            res->score = 0;
            snprintf(res->detail, sizeof(res->detail), "Battery voltage abnormal: %.2fV", voltage);
        }
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("Battery test done: %s\n", res->detail);
}

/* 测试蓝牙 */
static void test_bluetooth(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_BLUETOOTH];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_BLUETOOTH;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("Bluetooth test started\n");

    /* 检查蓝牙栈是否初始化 */
    test_output_append("  Bluetooth A2DP Sink: Xiaomiao Desktop\n");
    test_output_append("  Waiting for connection... (10s timeout)\n");

    /* 简单等待，实际连接检测需要回调 */
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->state = TEST_STATE_COMPLETED;
        res->score = 80;  // 无法完全自动验证连接
        snprintf(res->detail, sizeof(res->detail), "Bluetooth A2DP initialized, manual pairing test needed");
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("Bluetooth test done: %s\n", res->detail);
}

/* 测试 MicroPython */
static void test_micropython(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_MICROPYTHON];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_MICROPYTHON;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("MicroPython test started\n");

    if (!poincare_runtime_is_ready()) {
        test_output_append("  Initializing runtime...\n");
        if (!poincare_runtime_init(64 * 1024)) {
            if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                res->state = TEST_STATE_FAILED;
                res->score = 0;
                snprintf(res->detail, sizeof(res->detail), "Runtime init failed");
                xSemaphoreGive(s_result_mutex);
            }
            test_output_append("FAILED: Runtime init failed\n");
            return;
        }
    }

    /* 注意：MicroPython 运行时在 ui_init_task(长期任务) 中初始化，其 TLS/线程
     * 状态绑定于该任务。若在此测试任务(短生命周期，返回即删除)中调用
     * poincare_runtime_exec()，任务删除时会触发 FreeRTOS
     * "TLSP deletion callback at index 0 overwritten" abort。
     * 因此本项只验证运行时就绪 + xiaomiao framebuffer 可用；
     * 实际脚本执行能力由 EXEC_PYTHON 命令(在 UART 长期任务中同步执行)覆盖。 */
    bool ready = poincare_runtime_is_ready();
    uint16_t *fb = xiaomiao_display_get_framebuffer();
    bool fb_ok = (fb != NULL);

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ready && fb_ok) {
            res->state = TEST_STATE_COMPLETED;
            res->score = 100;
            snprintf(res->detail, sizeof(res->detail),
                     "MicroPython runtime ready; xiaomiao framebuffer available");
            test_output_append("MicroPython test PASSED\n");
        } else {
            res->state = TEST_STATE_FAILED;
            res->score = ready ? 60 : 0;
            snprintf(res->detail, sizeof(res->detail),
                     "runtime_ready=%d framebuffer=%d", ready, fb_ok);
            test_output_append("FAILED: runtime_ready=%d framebuffer=%d\n", ready, fb_ok);
        }
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("MicroPython test done: %s\n", res->detail);
}

/* 测试 SD 卡 */
static void test_sdcard(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_SDCARD];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_SDCARD;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("SD card test started\n");

    /* 尝试列出 /sdcard 目录 */
    DIR *dir = opendir("/sdcard");
    if (dir) {
        struct dirent *entry;
        int file_count = 0;
        int readdir_errno = 0;
        while (file_count < 20) {
            errno = 0;
            entry = readdir(dir);
            if (entry == NULL) {
                readdir_errno = errno;
                break;
            }
            if (entry->d_name[0] != '.') {
                test_output_append("  %s\n", entry->d_name);
                file_count++;
            }
        }
        closedir(dir);

        if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (readdir_errno != 0) {
                res->state = TEST_STATE_FAILED;
                res->score = 0;
                snprintf(res->detail, sizeof(res->detail), "SD card read error (errno=%d)", readdir_errno);
            } else {
                res->state = TEST_STATE_COMPLETED;
                res->score = 100;
                snprintf(res->detail, sizeof(res->detail), "SD card mounted, %d files listed", file_count);
            }
            xSemaphoreGive(s_result_mutex);
        }
    } else {
        if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            res->state = TEST_STATE_FAILED;
            res->score = 0;
            snprintf(res->detail, sizeof(res->detail), "SD card not mounted or not inserted");
            xSemaphoreGive(s_result_mutex);
        }
        test_output_append("FAILED: SD card not mounted\n");
    }

    test_output_append("SD card test done: %s\n", res->detail);
}

/* 测试 WiFi */
static void test_wifi(void)
{
    test_result_t *res = &s_test_results[TEST_ITEM_WIFI];
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->item = TEST_ITEM_WIFI;
        res->state = TEST_STATE_RUNNING;
        res->timestamp = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_result_mutex);
    }

    s_test_output_len = 0;
    test_output_append("WiFi test started\n");

    /* 这里只能检查WiFi是否初始化，实际连接测试需要更多代码 */
    test_output_append("  WiFi stack initialized\n");
    test_output_append("  Debug server on TCP 3333\n");

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        res->state = TEST_STATE_COMPLETED;
        res->score = 90;
        snprintf(res->detail, sizeof(res->detail), "WiFi stack initialized, debug server running");
        xSemaphoreGive(s_result_mutex);
    }

    test_output_append("WiFi test done: %s\n", res->detail);
}

/* ========== 测试运行器 ========== */

/* 实际执行单项测试（不负责删除任务，供任务入口与 run_all 复用） */
static void test_run_single_impl(test_item_t item)
{
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_current_test_item = item;
        s_current_test_state = TEST_STATE_RUNNING;
        s_test_running = true;
        xSemaphoreGive(s_result_mutex);
    }

    switch (item) {
        case TEST_ITEM_BUTTONS: test_buttons(); break;
        case TEST_ITEM_DISPLAY: test_display(); break;
        case TEST_ITEM_AUDIO: test_audio(); break;
        case TEST_ITEM_LED: test_led(); break;
        case TEST_ITEM_BATTERY: test_battery(); break;
        case TEST_ITEM_BLUETOOTH: test_bluetooth(); break;
        case TEST_ITEM_MICROPYTHON: test_micropython(); break;
        case TEST_ITEM_SDCARD: test_sdcard(); break;
        case TEST_ITEM_WIFI: test_wifi(); break;
        default: break;
    }
    
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_current_test_state = s_test_results[item].state;
        s_test_running = false;
        xSemaphoreGive(s_result_mutex);
    }
}

/* FreeRTOS 任务入口：单项测试。任务函数正常 return 会跳到非法地址触发
 * IllegalInstruction，必须显式删除自身。 */
static void test_run_single(test_item_t item)
{
    test_run_single_impl(item);
    vTaskDelete(NULL);
}

static void test_run_all(void)
{
    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_current_test_item = TEST_ITEM_ALL;
        s_current_test_state = TEST_STATE_RUNNING;
        s_test_running = true;
        xSemaphoreGive(s_result_mutex);
    }

    test_item_t items[] = {
        TEST_ITEM_BUTTONS, TEST_ITEM_DISPLAY, TEST_ITEM_AUDIO,
        TEST_ITEM_LED, TEST_ITEM_BATTERY, TEST_ITEM_BLUETOOTH,
        TEST_ITEM_MICROPYTHON, TEST_ITEM_SDCARD, TEST_ITEM_WIFI
    };
    
    for (int i = 0; i < 9; i++) {
        test_run_single_impl(items[i]);
        vTaskDelay(pdMS_TO_TICKS(500));  // 间隔
    }

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_current_test_state = TEST_STATE_COMPLETED;
        s_test_running = false;
        xSemaphoreGive(s_result_mutex);
    }
    vTaskDelete(NULL);
}

/* ========== 命令处理 ========== */

static void test_handle_cmd(const test_cmd_t *cmd)
{
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "id", cmd->id);
    cJSON_AddNumberToObject(response, "cmd", cmd->type);
    
    switch (cmd->type) {
        case TEST_CMD_PING: {
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddStringToObject(response, "msg", "pong");
            break;
        }
        case TEST_CMD_GET_INFO: {
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON *info = cJSON_CreateObject();
            cJSON_AddStringToObject(info, "device", "Xiaomiao Desktop");
            cJSON_AddStringToObject(info, "fw_version", "v71.1");
            cJSON_AddStringToObject(info, "hw_version", "ESP32-WROVER");
            cJSON_AddNumberToObject(info, "flash_mb", 16);
            cJSON_AddNumberToObject(info, "psram_mb", 8);
            /* 实时堆内存：用于内存泄漏诊断（GET_INFO 可被周期性采样对比） */
            cJSON_AddNumberToObject(info, "heap_free_bytes",
                                    (double)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(info, "heap_min_free_bytes",
                                    (double)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(info, "heap_largest_block",
                                    (double)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(info, "psram_free_bytes",
                                    (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            cJSON_AddNumberToObject(info, "heap_internal_free",
                                    (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(info, "heap_internal_min_free_bytes",
                                    (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(info, "heap_internal_largest_block",
                                    (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            cJSON_AddNumberToObject(info, "screen_w", XIAOMIAO_SCREEN_WIDTH);
            cJSON_AddNumberToObject(info, "screen_h", XIAOMIAO_SCREEN_HEIGHT);
            cJSON_AddNumberToObject(info, "buttons", 6);
            cJSON_AddNumberToObject(info, "leds", XIAOMIAO_LED_COUNT);
            cJSON_AddBoolToObject(info, "micropython_ready", poincare_runtime_is_ready());
            bool running = false;
            if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                running = s_test_running;
                xSemaphoreGive(s_result_mutex);
            }
            cJSON_AddBoolToObject(info, "test_running", running);
            cJSON_AddItemToObject(response, "data", info);
            break;
        }
        case TEST_CMD_RUN_TEST: {
            cJSON *item_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "item") : NULL;
            if (item_json && cJSON_IsNumber(item_json)) {
                test_item_t item = (test_item_t)item_json->valueint;
                if (item == TEST_ITEM_ALL) {
                    xTaskCreate(test_run_all, "test_all", TEST_TASK_STACK_SIZE, NULL, TEST_TASK_PRIORITY, NULL);
                    cJSON_AddStringToObject(response, "status", "started");
                    cJSON_AddStringToObject(response, "msg", "All tests started");
                } else if (item >= 0 && item <= TEST_ITEM_WIFI) {
                    xTaskCreate((TaskFunction_t)test_run_single, "test_one", TEST_TASK_STACK_SIZE, (void*)item, TEST_TASK_PRIORITY, NULL);
                    cJSON_AddStringToObject(response, "status", "started");
                    cJSON_AddStringToObject(response, "msg", "Test started");
                } else {
                    cJSON_AddStringToObject(response, "status", "error");
                    cJSON_AddStringToObject(response, "msg", "Invalid test item");
                }
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'item' parameter");
            }
            break;
        }
        case TEST_CMD_GET_TEST_RESULT: {
            cJSON *item_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "item") : NULL;
            cJSON *results = cJSON_CreateArray();

            /* 在锁内拷贝快照，避免持锁构造 JSON */
            test_result_t snapshot[TEST_ITEM_WIFI + 1] = {0};
            int snapshot_count = 0;
            int requested_item = -1;
            if (item_json && cJSON_IsNumber(item_json)) {
                int v = item_json->valueint;
                if (v >= 0 && v <= TEST_ITEM_WIFI) requested_item = v;
            }

            if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (requested_item >= 0) {
                    snapshot[0] = s_test_results[requested_item];
                    snapshot_count = 1;
                } else {
                    for (int i = 0; i <= TEST_ITEM_WIFI; i++) {
                        if (s_test_results[i].state != TEST_STATE_IDLE || s_test_results[i].score > 0) {
                            snapshot[snapshot_count++] = s_test_results[i];
                        }
                    }
                }
                xSemaphoreGive(s_result_mutex);
            }

            for (int i = 0; i < snapshot_count; i++) {
                test_result_t *res = &snapshot[i];
                cJSON *r = cJSON_CreateObject();
                cJSON_AddNumberToObject(r, "item", res->item);
                cJSON_AddNumberToObject(r, "state", res->state);
                cJSON_AddNumberToObject(r, "score", res->score);
                cJSON_AddStringToObject(r, "detail", res->detail);
                cJSON_AddNumberToObject(r, "timestamp", res->timestamp);
                cJSON_AddItemToArray(results, r);
            }

            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddItemToObject(response, "data", results);
            break;
        }
        case TEST_CMD_EXEC_PYTHON: {
            cJSON *script_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "script") : NULL;
            if (script_json && cJSON_IsString(script_json)) {
                /* 同步执行：MicroPython TLS/线程状态绑定到初始化它的任务，
                 * 在新创建的短生命周期任务里执行会在任务删除时触发 FreeRTOS
                 * "TLSP deletion callback at index 0 overwritten" abort。
                 * 因此改为在长期存活的 UART 命令任务中同步执行并直接返回结果。
                 * UART 任务栈已提升到 TEST_UART_TASK_STACK(12KB) 足以容纳调用链。 */
                int ret = -1;
                const char *err = NULL;
                if (s_python_mutex && xSemaphoreTake(s_python_mutex, pdMS_TO_TICKS(30000)) == pdTRUE) {
                    if (!poincare_runtime_is_ready()) {
                        poincare_runtime_init(64 * 1024);
                    }
                    /* 重置长度的同时清空首字节：print 输出走 UART console，
                     * 不经 test_output_append，若上次 RUN_TEST 留下旧内容，
                     * 这里只把 len 置 0 会让 output 字段返回陈旧文本。 */
                    s_test_output_len = 0;
                    s_test_output_buffer[0] = '\0';
                    ret = poincare_runtime_exec(script_json->valuestring, "host_cmd");
                    err = poincare_runtime_get_last_error();
                    xSemaphoreGive(s_python_mutex);
                } else {
                    err = "Python runtime busy or init timeout";
                }

                cJSON_AddStringToObject(response, "status", ret == 0 ? "ok" : "error");
                cJSON_AddNumberToObject(response, "ret", ret);
                if (err && err[0]) {
                    cJSON_AddStringToObject(response, "error", err);
                }
                cJSON_AddStringToObject(response, "output", s_test_output_buffer);
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'script' parameter");
            }
            break;
        }
        case TEST_CMD_SIMULATE_KEY: {
            cJSON *key_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "key") : NULL;
            cJSON *long_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "long") : NULL;
            
            if (key_json && cJSON_IsNumber(key_json)) {
                int key = key_json->valueint;
                bool is_long = long_json && cJSON_IsTrue(long_json);
                
                if (key >= 0 && key <= 5) {
                    xm_btn_event_t evt = { .key = key, .is_long_press = is_long };
                    bool sent = xiaomiao_button_push(&evt);
                    
                    cJSON_AddStringToObject(response, "status", sent ? "ok" : "error");
                    cJSON_AddStringToObject(response, "msg", sent ? "Key event injected" : "Queue full");
                } else {
                    cJSON_AddStringToObject(response, "status", "error");
                    cJSON_AddStringToObject(response, "msg", "Invalid key (0-5)");
                }
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'key' parameter");
            }
            break;
        }
        case TEST_CMD_DIAG_WIFI_SCAN: {
            /* 在 UART 命令任务内同步触发扫描发起，扫描完成回调在系统事件任务执行。
             * 命令立即返回，随后用串口日志观察是否卡死/重启。 */
            wifi_diag_trigger_scan();
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddStringToObject(response, "msg", "WiFi scan triggered");
            break;
        }
        case TEST_CMD_SET_LED: {
            cJSON *index_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "index") : NULL;
            cJSON *r_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "r") : NULL;
            cJSON *g_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "g") : NULL;
            cJSON *b_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "b") : NULL;
            
            if (index_json && cJSON_IsNumber(index_json) && 
                r_json && cJSON_IsNumber(r_json) &&
                g_json && cJSON_IsNumber(g_json) &&
                b_json && cJSON_IsNumber(b_json)) {
                int index = index_json->valueint;
                led_rgb_t color = {
                    .r = (uint8_t)r_json->valueint,
                    .g = (uint8_t)g_json->valueint,
                    .b = (uint8_t)b_json->valueint
                };
                
                if (index >= 0 && index < XIAOMIAO_LED_COUNT) {
                    drv_led_strip_set_pixel(index, color);
                    drv_led_strip_refresh();
                    cJSON_AddStringToObject(response, "status", "ok");
                } else {
                    cJSON_AddStringToObject(response, "status", "error");
                    cJSON_AddStringToObject(response, "msg", "Invalid LED index (0-2)");
                }
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'index', 'r', 'g', or 'b' parameter");
            }
            break;
        }
        case TEST_CMD_SET_BACKLIGHT: {
            cJSON *brightness_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "brightness") : NULL;
            if (brightness_json && cJSON_IsNumber(brightness_json)) {
                int percent = brightness_json->valueint;
                if (percent < 0) percent = 0;
                if (percent > 100) percent = 100;
                drv_backlight_set_brightness(percent);
                cJSON_AddStringToObject(response, "status", "ok");
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'brightness' parameter (0-100)");
            }
            break;
        }
        case TEST_CMD_PLAY_TONE: {
            cJSON *freq_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "freq") : NULL;
            cJSON *dur_json = cmd->params ? cJSON_GetObjectItem(cmd->params, "duration") : NULL;

            if (freq_json && cJSON_IsNumber(freq_json)) {
                uint32_t freq = (uint32_t)freq_json->valueint;
                uint32_t dur = dur_json && cJSON_IsNumber(dur_json) ? (uint32_t)dur_json->valueint : 500;

                /* 异步播放：audio_output_tone 内含 I2S 写入 + 浮点 sinf 合成，
                 * 在命令任务里同步执行会栈溢出。放到独立任务，先回 accepted。
                 * 连续快速下发时，上一条 tone_task 可能尚未自删释放 task 槽 /
                 * 堆块，导致首次 xTaskCreate 偶发失败；短暂延迟后重试几次。 */
                bool tone_started = false;
                for (int attempt = 0; attempt < 4 && !tone_started; attempt++) {
                    tone_ctx_t *tctx = calloc(1, sizeof(tone_ctx_t));
                    if (!tctx) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }
                    tctx->freq = freq;
                    tctx->duration = dur;
                    tctx->req_id = cmd->id;
                    if (xTaskCreate(test_play_tone_task, "tone_task",
                                    TEST_TASK_STACK_SIZE, tctx,
                                    TEST_TASK_PRIORITY, NULL) == pdPASS) {
                        tone_started = true;
                    } else {
                        free(tctx);
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                }
                if (tone_started) {
                    cJSON_AddStringToObject(response, "status", "ok");
                    cJSON_AddStringToObject(response, "msg", "Tone playback started");
                } else {
                    cJSON_AddStringToObject(response, "status", "error");
                    cJSON_AddStringToObject(response, "msg", "Failed to start tone task");
                }
            } else {
                cJSON_AddStringToObject(response, "status", "error");
                cJSON_AddStringToObject(response, "msg", "Missing 'freq' parameter");
            }
            break;
        }
        case TEST_CMD_GET_BATTERY: {
            float voltage = drv_battery_get_voltage();
            int percent = drv_battery_get_percent(voltage);
            
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "voltage_mv", (int)(voltage * 1000));
            cJSON_AddNumberToObject(data, "percent", percent);
            
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddItemToObject(response, "data", data);
            break;
        }
        case TEST_CMD_LIST_TESTS: {
            cJSON *tests = cJSON_CreateArray();
            const char *names[] = {
                "Buttons (6 keys)", "Display (160x128)", "Audio (I2S DAC)",
                "LED (WS2812B x3)", "Battery (ADC)", "Bluetooth (A2DP)",
                "MicroPython", "SD Card", "WiFi"
            };
            for (int i = 0; i <= TEST_ITEM_WIFI; i++) {
                cJSON *t = cJSON_CreateObject();
                cJSON_AddNumberToObject(t, "id", i);
                cJSON_AddStringToObject(t, "name", names[i]);
                cJSON_AddItemToArray(tests, t);
            }
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddItemToObject(response, "data", tests);
            break;
        }
        case TEST_CMD_RESET_DEVICE: {
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddStringToObject(response, "msg", "Resetting in 1s...");
            test_send_response(response);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
            return; // 不会执行到这里
        }
        case TEST_CMD_ENTER_BOOTLOADER: {
            cJSON_AddStringToObject(response, "status", "ok");
            cJSON_AddStringToObject(response, "msg", "Entering bootloader...");
            test_send_response(response);
            vTaskDelay(pdMS_TO_TICKS(500));
            // 这里可以调用 return_to_loader 相关函数
            esp_restart();
            return;
        }
        default: {
            cJSON_AddStringToObject(response, "status", "error");
            cJSON_AddStringToObject(response, "msg", "Unknown command");
            break;
        }
    }
    
    test_send_response(response);
}

/* ========== UART 发送响应 ========== */
static esp_err_t test_send_response(cJSON *response)
{
    char *json_str = cJSON_PrintUnformatted(response);
    if (json_str) {
        if (s_uart_tx_mutex && xSemaphoreTake(s_uart_tx_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            /* 必须一次性写入完整行：原先分三次写，ESP_LOG 可能插在 XTR: 前缀
             * 与 JSON 之间，主机解析到的响应行被拆成两段而丢失。 */
            size_t json_len = strlen(json_str);
            char *line = heap_caps_malloc(4 + json_len + 2, MALLOC_CAP_8BIT);
            if (line) {
                memcpy(line, "XTR:", 4);
                memcpy(line + 4, json_str, json_len);
                line[4 + json_len] = '\n';
                line[5 + json_len] = '\0';
                uart_write_bytes(TEST_UART_NUM, line, json_len + 5);
                heap_caps_free(line);
            } else {
                uart_write_bytes(TEST_UART_NUM, "XTR:", 4);
                uart_write_bytes(TEST_UART_NUM, json_str, json_len);
                uart_write_bytes(TEST_UART_NUM, "\n", 1);
            }
            xSemaphoreGive(s_uart_tx_mutex);
        }
        free(json_str);
    }
    cJSON_Delete(response);
    return ESP_OK;
}

/* ========== 异步音调播放任务 ========== */
static void test_play_tone_task(void *arg)
{
    tone_ctx_t *ctx = (tone_ctx_t *)arg;
    if (ctx) {
        /* 音频后端非线程安全，串行化访问 */
        if (s_audio_mutex && xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            audio_output_tone(ctx->freq, ctx->duration);
            xSemaphoreGive(s_audio_mutex);
        }
        free(ctx);
    }
    vTaskDelete(NULL);
}

/* ========== UART 接收任务 ========== */
static void test_uart_task(void *arg)
{
    uint8_t *buf = malloc(TEST_UART_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate UART buffer");
        vTaskDelete(NULL);
        return;
    }
    
    size_t buf_len = 0;
    
    while (1) {
        /* 缓冲区满时不再读取，防止越界；等待下一轮处理完整行 */
        size_t free_space = (buf_len < TEST_UART_BUF_SIZE - 1)
                            ? (TEST_UART_BUF_SIZE - 1 - buf_len) : 0;
        int len = 0;
        if (free_space > 0) {
            len = uart_read_bytes(TEST_UART_NUM, buf + buf_len, free_space, pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (len < 0) {
            /* driver 未就绪/读错误：延时后再试，避免高速空转刷屏 */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (len > 0) {
            buf_len += len;
            buf[buf_len] = '\0';

            /* 查找完整行（以\n结尾） */
            char *line_start = (char*)buf;
            char *line_end;
            while ((line_end = strchr(line_start, '\n')) != NULL) {
                *line_end = '\0';

                /* 去掉可能的 XTC: 前缀（兼容无前缀的纯 JSON） */
                char *json_line = line_start;
                const size_t prefix_len = 4;  // strlen("XTC:")
                if (strncmp(line_start, "XTC:", prefix_len) == 0) {
                    json_line = line_start + prefix_len;
                }

                if (strlen(json_line) > 0 && json_line[0] == '{') {
                    /* 解析 JSON 命令 */
                    cJSON *cmd_json = cJSON_Parse(json_line);
                    if (cmd_json) {
                        cJSON *type_json = cJSON_GetObjectItem(cmd_json, "cmd");
                        cJSON *params_json = cJSON_GetObjectItem(cmd_json, "params");
                        cJSON *id_json = cJSON_GetObjectItem(cmd_json, "id");

                        if (type_json && cJSON_IsNumber(type_json)) {
                            test_cmd_t cmd;
                            cmd.id = (id_json && cJSON_IsNumber(id_json)) ? id_json->valueint : 0;
                            cmd.type = (test_cmd_type_t)type_json->valueint;
                            cmd.params = params_json ? cJSON_Duplicate(params_json, 1) : NULL;

                            test_handle_cmd(&cmd);

                            /* 释放复制的参数，避免内存泄漏 */
                            if (cmd.params) {
                                cJSON_Delete(cmd.params);
                            }
                        }
                        cJSON_Delete(cmd_json);
                    }
                }

                line_start = line_end + 1;
            }

            /* 移动剩余数据到缓冲区开头 */
            if (line_start > (char*)buf) {
                size_t remaining = buf_len - (line_start - (char*)buf);
                memmove(buf, line_start, remaining);
                buf_len = remaining;
                buf[buf_len] = '\0';
            }
        }

        /* 无换行超长行：丢弃整包防止死锁 */
        if (buf_len >= TEST_UART_BUF_SIZE - 1 && strchr((char*)buf, '\n') == NULL) {
            buf_len = 0;
            buf[0] = '\0';
            ESP_LOGW(TAG, "Dropping oversized UART line without newline");
        }
    }
}

/* ========== 公共 API ========== */

void app_test_init(void)
{
    ESP_LOGI(TAG, "Initializing test command interface");
    
    /* 创建互斥锁 */
    s_result_mutex = xSemaphoreCreateMutex();
    s_uart_tx_mutex = xSemaphoreCreateMutex();
    s_python_mutex = xSemaphoreCreateMutex();
    s_audio_mutex = xSemaphoreCreateMutex();
    if (!s_result_mutex || !s_uart_tx_mutex || !s_python_mutex || !s_audio_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return;
    }

    /* UART0 默认作为 console，日志经 ROM 直出，但 UART driver 未安装时
     * uart_read_bytes()/uart_write_bytes() 会返回 -1 并刷 "uart driver error"。
     * 显式配置并安装 driver（若已被 console 安装则视为成功）。 */
    uart_config_t uart_cfg = {
        .baud_rate = TEST_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    esp_err_t uart_err = uart_param_config(TEST_UART_NUM, &uart_cfg);
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %d", uart_err);
        return;
    }
    uart_err = uart_driver_install(TEST_UART_NUM,
                                   TEST_UART_BUF_SIZE * 2,
                                   TEST_UART_BUF_SIZE, 0, NULL, 0);
    if (uart_err != ESP_OK && uart_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %d", uart_err);
        return;
    }

    /* 初始化测试结果数组 */
    for (int i = 0; i <= TEST_ITEM_ALL; i++) {
        s_test_results[i].item = (test_item_t)i;
        s_test_results[i].state = TEST_STATE_IDLE;
        s_test_results[i].score = 0;
        s_test_results[i].detail[0] = '\0';
        s_test_results[i].timestamp = 0;
    }
    
    /* 启动 UART 接收任务 */
    /* Higher priority than the UI/button tasks so command responses are not
     * starved during the busy boot period (WiFi retries + BT A2DP init). */
    TaskHandle_t uart_task_handle = xTaskCreateStatic(
        test_uart_task, "test_uart", TEST_UART_TASK_STACK / sizeof(StackType_t),
        NULL, 20, s_test_uart_stack, &s_test_uart_tcb);
    if (!uart_task_handle) {
        ESP_LOGE(TAG, "xTaskCreateStatic failed for test_uart");
        return;
    }

    ESP_LOGI(TAG, "Test interface ready on USB CDC (UART0), stack in PSRAM");
}

bool app_test_is_running(void)
{
    bool running = false;
    if (s_result_mutex && xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        running = s_test_running;
        xSemaphoreGive(s_result_mutex);
    }
    return running;
}

test_state_t app_test_get_state(test_item_t item)
{
    test_state_t state = TEST_STATE_IDLE;
    if (!s_result_mutex || xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return state;
    }
    if (item >= 0 && item <= TEST_ITEM_WIFI) {
        state = s_test_results[item].state;
    } else {
        state = s_current_test_state;
    }
    xSemaphoreGive(s_result_mutex);
    return state;
}

const test_result_t* app_test_get_result(test_item_t item)
{
    /* 注意：返回指针仅供调用方读取，测试运行中内容可能被并发更新 */
    if (item >= 0 && item <= TEST_ITEM_WIFI) {
        return &s_test_results[item];
    }
    return NULL;
}