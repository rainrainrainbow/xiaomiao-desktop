/**
 * @file app_micropython.h
 * @brief MicroPython 运行时集成接口
 * 
 * 提供 MicroPython 运行时初始化和脚本执行引擎。
 */

#ifndef APP_MICROPYTHON_H
#define APP_MICROPYTHON_H

#include "app_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== MicroPython 运行时 API ========== */

/**
 * @brief 初始化 MicroPython 运行时（幂等，可重复调用）
 * @return true 成功，false 失败
 */
bool app_micropython_init(void);

/**
 * @brief 检查 MicroPython 是否已初始化
 * @return true 已初始化
 */
bool app_micropython_is_ready(void);

/**
 * @brief 执行一段 Python 源码字符串
 * @param source Python 源码
 * @param source_name 源码名称（用于错误报告），可为 NULL
 * @return 0 成功，-1 失败
 */
int app_micropython_exec(const char *source, const char *source_name);

/**
 * @brief 执行一个 Python 文件
 * @param filename 文件路径
 * @return 0 成功，-1 失败
 */
int app_micropython_exec_file(const char *filename);

/* ========== MicroPython 应用页面回调 ========== */

/**
 * @brief 获取 MicroPython 应用页面回调
 * @return 页面回调结构体指针
 */
const page_callbacks_t* app_micropython_get_callbacks(void);

/**
 * @brief 扫描 SD 卡中的 MicroPython 应用
 * @param base_path 基础路径
 * @param apps 应用数组输出
 * @param max_count 最大应用数
 * @return 扫描到的应用数量
 */
int app_micropython_scan(const char *base_path, app_def_t *apps, int max_count);

/* ========== Python 应用运行支持（v66：屏幕/按键/时间绑定） ========== */

/**
 * @brief 注册/管理 Python 应用的上屏刷新（main 循环周期调用）
 *
 * Python 应用在独立任务中运行，通过 xiaomiao 模块的 framebuffer 绘制画面。
 * 当 Python 调用 show() 时，flush 回调只设置标志（不直接操作 LVGL，
 * 因为 LVGL 必须在 main 任务上下文操作）。本函数由 main 循环每次迭代调用，
 * 检测到脏标志后执行 canvas 无效化 + 强制刷新，实现线程安全上屏。
 */
void app_micropython_on_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MICROPYTHON_H */