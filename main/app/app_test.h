/**
 * @file app_test.h
 * @brief 自动化测试命令接口头文件
 */

#ifndef APP_TEST_H
#define APP_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_button.h"
#include "poincare/mp_xiaomiao.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 测试项目枚举 ========== */
typedef enum {
    TEST_ITEM_BUTTONS = 0,      // 按键测试（6键）
    TEST_ITEM_DISPLAY,          // 屏幕显示测试
    TEST_ITEM_AUDIO,            // 音频测试（I2S DAC）
    TEST_ITEM_LED,              // LED测试（WS2812B x3）
    TEST_ITEM_BATTERY,          // 电池电压测试
    TEST_ITEM_BLUETOOTH,        // 蓝牙A2DP测试
    TEST_ITEM_MICROPYTHON,      // MicroPython运行时测试
    TEST_ITEM_SDCARD,           // SD卡测试
    TEST_ITEM_WIFI,             // WiFi测试
    TEST_ITEM_ALL,              // 全部测试
} test_item_t;

/* ========== 测试状态枚举 ========== */
typedef enum {
    TEST_STATE_IDLE = 0,
    TEST_STATE_RUNNING,
    TEST_STATE_COMPLETED,
    TEST_STATE_FAILED,
} test_state_t;

/* ========== 测试结果结构 ========== */
typedef struct {
    test_item_t item;
    test_state_t state;
    int score;              // 0-100
    char detail[256];       // 详细信息
    uint32_t timestamp;     // 完成时间戳（毫秒）
} test_result_t;

/* ========== 公共 API ========== */

/**
 * @brief 初始化测试命令接口
 * 启动 USB CDC 串口监听任务，接收 JSON 格式命令
 */
void app_test_init(void);

/**
 * @brief 检查测试是否正在运行
 * @return true 正在运行
 */
bool app_test_is_running(void);

/**
 * @brief 获取测试状态
 * @param item 测试项目
 * @return 测试状态
 */
test_state_t app_test_get_state(test_item_t item);

/**
 * @brief 获取测试结果详情
 * @param item 测试项目
 * @return 测试结果指针（仅在测试完成后有效）
 */
const test_result_t* app_test_get_result(test_item_t item);

/* ========== 命令协议说明 ========== */
/*
 * 命令与响应均为单行文本，以 \n 结尾：
 *  - 命令以 "XTC:" 前缀开头（兼容无前缀的纯 JSON）
 *  - 响应以 "XTR:" 前缀开头，便于与 ESP32 普通日志区分
 *
 * 命令格式：
 * XTC:{"cmd": <命令码>, "id": <请求ID>, "params": {...}}
 *
 * 响应格式：
 * XTR:{"id": <请求ID>, "cmd": <命令码>, "status": "ok|error|accepted", "msg": "...", "data": {...}}
 *
 * 命令码：
 * 0  - TEST_CMD_PING              无参数，返回 pong
 * 1  - TEST_CMD_GET_INFO          无参数，返回设备信息
 * 2  - TEST_CMD_RUN_TEST          params: {"item": <test_item_t>} 启动测试
 * 3  - TEST_CMD_GET_TEST_RESULT   params: {"item": <test_item_t>?} 获取结果
 * 4  - TEST_CMD_EXEC_PYTHON       params: {"script": "<python代码>"} 同步执行，
 *                                 直接返回 status=ok|error 与 ret/error。
 *                                 注意：本板级 MicroPython 未把 print() 路由到
 *                                 任何 console（应用输出走 framebuffer），故
 *                                 output 字段通常为空；执行成功以 ret=0 为准，
 *                                 解释器/模块可用性由 RUN_TEST item6 单独验证。
 * 5  - TEST_CMD_SIMULATE_KEY      params: {"key": 0-5, "long": true/false} 模拟按键
 * 6  - TEST_CMD_SET_LED           params: {"index": 0-2, "r": 0-255, "g": 0-255, "b": 0-255} 设置LED
 * 7  - TEST_CMD_SET_BACKLIGHT     params: {"brightness": 0-100} 设置背光百分比
 * 8  - TEST_CMD_PLAY_TONE         params: {"freq": Hz, "duration": ms} 播放音调
 * 9  - TEST_CMD_GET_BATTERY       无参数，返回电池电压和电量
 * 10 - TEST_CMD_LIST_TESTS        无参数，返回可用测试列表
 * 11 - TEST_CMD_RESET_DEVICE      无参数，复位设备
 * 12 - TEST_CMD_ENTER_BOOTLOADER  无参数，进入下载模式
 * 13 - TEST_CMD_DIAG_WIFI_SCAN    无参数，直接触发 WiFi 扫描（诊断用）
 *
 * 测试项目：
 * 0 - Buttons, 1 - Display, 2 - Audio, 3 - LED, 4 - Battery,
 * 5 - Bluetooth, 6 - MicroPython, 7 - SDCard, 8 - WiFi, 9 - All
 */

#ifdef __cplusplus
}
#endif

#endif /* APP_TEST_H */