/*
 * This file is part of the XiaoMiao Desktop project.
 *
 * MicroPython 扩展模块 xiaomiao 的头文件。
 *
 * 提供 C 接口，供主项目（main 组件）接入：
 * - xiaomiao_display_init()             初始化/获取 framebuffer
 * - xiaomiao_display_set_flush_cb()     注册上屏回调（由主项目实现）
 * - xiaomiao_display_get_framebuffer()  获取 RGB565 SWAPPED framebuffer 指针
 * - xiaomiao_button_push()              向 Python 按键队列注入按键事件
 * - xiaomiao_button_task_is_active()    查询 Python 按键消费者是否活跃
 *
 * 设计说明：
 * - framebuffer：160x128x2 = 40KB，从 PSRAM 分配（RGB565 SWAPPED 字节序，与 LCD 一致）。
 *   主项目可用 LVGL canvas 零拷贝承接该缓冲区，或直接将其作为 LVGL draw buffer。
 * - 上屏：Python 脚本调用 xiaomiao.show() 时触发注册的 flush 回调。
 * - 按键：Python 脚本调用 xiaomiao.get_key() 时从内部队列读取。
 *   主项目（main 循环）通过 xiaomiao_button_push() 将按键事件转发给 Python。
 */

#ifndef MP_XIAOMIAO_H
#define MP_XIAOMIAO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 屏幕尺寸（与 LCD_H_RES / LCD_V_RES 一致：横屏 160x128） */
#define XM_SCREEN_W  160
#define XM_SCREEN_H  128

/* 按键事件结构（与 drv_button.h 的 btn_event_t 字段一致） */
typedef struct {
    int key;            /* 按键索引 (0-5)：UP/DOWN/LEFT/RIGHT/A/B */
    bool is_long_press; /* 是否为长按 */
} xm_btn_event_t;

/* 按键读取回调类型：由主项目（main 组件）注册。
 * 注意：不能用 extern 直接引用 main 组件的符号（会造成组件循环依赖
 * main → micropython → main），因此采用回调注入解耦。 */
typedef bool (*xm_btn_read_fn)(xm_btn_event_t *evt);

/* 上屏回调类型：由主项目注册（如 LVGL canvas 承接 framebuffer 后强制刷新） */
typedef void (*xm_flush_fn)(void);

/**
 * @brief 初始化 xiaomiao framebuffer（幂等；从 PSRAM 分配 40KB）
 * @return true 成功
 */
bool xiaomiao_display_init(void);

/**
 * @brief 注册上屏回调
 * @param cb 回调函数（可传 NULL 取消）
 */
void xiaomiao_display_set_flush_cb(xm_flush_fn cb);

/**
 * @brief 注册按键读取回调（由 main 组件注入 drv_button_get_event）
 * @param cb 回调函数（可传 NULL 取消）
 */
void xiaomiao_button_set_read_cb(xm_btn_read_fn cb);

/**
 * @brief 获取 framebuffer 指针（RGB565 SWAPPED，160x128）
 * @return 指针；未初始化时返回 NULL
 */
uint16_t *xiaomiao_display_get_framebuffer(void);

/**
 * @brief 向 Python 按键队列推送事件（供主循环转发按键）
 * @param evt 按键事件
 * @return true 已入队
 */
bool xiaomiao_button_push(const xm_btn_event_t *evt);

/**
 * @brief 清空 Python 按键队列（启动新脚本前调用，丢弃残留按键）
 */
void xiaomiao_button_flush(void);

/**
 * @brief 请求停止当前 Python 脚本（协作式）
 *
 * 置停止标志；Python 脚本下一次调用 xiaomiao.get_key() 时会抛出
 * KeyboardInterrupt，脚本捕获后自然退出（pyexec_file 返回，任务结束）。
 * 用于 B 键返回时安全终止游戏循环，避免强制杀任务破坏 MicroPython 状态。
 */
void xiaomiao_request_stop(void);

/**
 * @brief 查询 Python 按键消费者是否活跃（即 Python 脚本曾调用 get_key）
 * @return true 活跃
 */
bool xiaomiao_button_task_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MP_XIAOMIAO_H */