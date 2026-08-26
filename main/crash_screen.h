/**
 * @file crash_screen.h
 * @brief 崩溃蓝屏显示模块接口
 */

#pragma once

#include "esp_lcd_panel_io.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 崩溃信息结构
 */
typedef struct {
    uint32_t pc;          // 程序计数器
    uint32_t excvaddr;    // 异常虚拟地址
    uint8_t exccause;     // 异常原因
    const char *task_name; // 崩溃时的任务名
} crash_info_t;

/**
 * @brief 设置 LCD IO 句柄
 * 
 * 在 LCD 初始化后调用，供崩溃蓝屏使用
 * 
 * @param io LCD panel IO 句柄
 */
void crash_screen_set_lcd_io(esp_lcd_panel_io_handle_t io);

/**
 * @brief 显示崩溃蓝屏
 * 
 * 在 panic handler 中调用，直接在 LCD 上显示崩溃信息
 * 不依赖 LVGL，使用底层 LCD 命令
 * 
 * @param info 崩溃信息
 */
void crash_screen_show(const crash_info_t *info);

/**
 * @brief 显示崩溃蓝屏并重启
 * 
 * 在检测到关键错误（如内存耗尽）时调用
 * 显示蓝屏信息后等待 5 秒重启
 * 
 * @param info 崩溃信息
 */
void crash_screen_show_and_restart(const crash_info_t *info);

#ifdef __cplusplus
}
#endif