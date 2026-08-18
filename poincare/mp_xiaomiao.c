/*
 * This file is part of the XiaoMiao Desktop project.
 *
 * MicroPython 扩展模块：xiaomiao
 *
 * 为 MicroPython 脚本提供屏幕绘制、按键读取、时间控制等系统绑定，
 * 使 Python 应用（如贪吃蛇）能够在真机屏幕上实时渲染并交互。
 *
 * 架构（解耦设计）：
 * - framebuffer：本模块从 PSRAM 分配 160x128x2 = 40KB 缓冲区，
 *   Python 脚本可直接绘制（RGB565 SWAPPED 字节序，与 LCD 一致）。
 * - 上屏刷新：通过注册的回调函数 s_flush_cb 交给主项目实现
 *   （主项目用 LVGL canvas 承接 framebuffer 并强制刷新，见 mp_xiaomiao.h）。
 * - 按键读取：复用 main 组件 drv_button_get_event()（链接阶段解析），
 *   非阻塞读取按键事件队列。
 * - 时间控制：esp_timer + vTaskDelay。
 *
 * 颜色：24-bit 0xRRGGBB（Python 传整数），内部转 RGB565。
 *
 * 模块注册：使用 MP_REGISTER_MODULE(MP_QSTR_xiaomiao, xiaomiao_module)
 * 需要确保 MP_QSTR_xiaomiao 被 qstr 收集（见 qstrdefsport.h 或自动收集）。
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mphal.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "poincare/mp_xiaomiao.h"

/* ========== 屏幕常量 ========== */
#define XM_SCREEN_W  160
#define XM_SCREEN_H  128

/* ========== 按键常量（与 ui_framework.h 一致） ========== */
enum {
    XM_KEY_UP = 0,
    XM_KEY_DOWN,
    XM_KEY_LEFT,
    XM_KEY_RIGHT,
    XM_KEY_A,
    XM_KEY_B,
    XM_KEY_NONE = -1,
};

/* ========== framebuffer 与 flush 回调 ========== */
static uint16_t *s_fb = NULL;          /* 40KB PSRAM RGB565缓冲区 */
static xm_flush_fn s_flush_cb = NULL;  /* 由主项目注册 */

/* ========== 按键读取（回调注入，由 main 组件注册 drv_button_get_event） ========== */
/*
 * 按键读取回调由 main 组件在启动时通过 xiaomiao_button_set_read_cb 注入，
 * 避免 micropython 组件直接 extern main 组件的符号造成组件循环依赖
 * （main → micropython → main）。链接时仍由主固件统一解析。
 */
static xm_btn_read_fn s_btn_read_cb = NULL;

void xiaomiao_button_set_read_cb(xm_btn_read_fn cb)
{
    s_btn_read_cb = cb;
}

/* ========== 按键转发队列（主循环 → Python） ========== */
/*
 * 关键：Python 应用（如贪吃蛇）运行时，游戏主循环在独立任务中执行，
 * 按键事件不能直接读 drv_button 队列——因为 main 循环也会从同一队列消费
 * （用于 UI 导航）。若两边同时读同一队列，事件会被抢走。
 *
 * 因此设计两级按键通路：
 * 1. main 循环从 drv_button_get_event() 消费（保持现有 UI 导航逻辑不变）；
 * 2. 当 Python 应用处于激活态（xiaomiao_button_task_is_active()），
 *    main 循环将按键同时转发到本模块的内部队列 s_py_btn_queue；
 * 3. Python 脚本通过 xiaomiao.get_key() 从 s_py_btn_queue 读取。
 *
 * 这样 Python 游戏既能读到按键，又不与 UI 导航抢事件。
 */
static QueueHandle_t s_py_btn_queue = NULL;
static volatile bool s_py_btn_active = false;

static void xm_btn_queue_ensure(void)
{
    if (s_py_btn_queue == NULL) {
        s_py_btn_queue = xQueueCreate(8, sizeof(xm_btn_event_t));
    }
}

bool xiaomiao_button_push(const xm_btn_event_t *evt)
{
    if (!evt) return false;
    xm_btn_queue_ensure();
    if (s_py_btn_queue == NULL) return false;
    return xQueueSend(s_py_btn_queue, evt, 0) == pdTRUE;
}

bool xiaomiao_button_task_is_active(void)
{
    return s_py_btn_active;
}

void xiaomiao_button_flush(void)
{
    xm_btn_queue_ensure();
    if (s_py_btn_queue) {
        xm_btn_event_t evt;
        while (xQueueReceive(s_py_btn_queue, &evt, 0) == pdTRUE) {
            /* 丢弃 */
        }
    }
}

/* ========== 协作式停止（B键返回时安全终止 Python 脚本） ========== */
/*
 * Python 脚本运行在独立任务中，无法从外部强制杀死（会破坏 MicroPython
 * 运行时状态）。采用协作式停止：
 * - python_app_destroy() 调用 xiaomiao_request_stop() 置停止标志；
 * - Python 脚本的 get_key() 调用检测到标志后抛出 KeyboardInterrupt，
 *   脚本顶层 nlr 捕获后自然退出，pyexec_file 返回，任务正常结束。
 * 这要求 Python 脚本周期性调用 get_key()（贪吃蛇主循环天然满足）。
 */
static volatile bool s_stop_requested = false;

void xiaomiao_request_stop(void)
{
    s_stop_requested = true;
}

/* Python 侧检查并清除停止标志（返回 true 表示应停止） */
static bool xm_stop_check(void)
{
    if (s_stop_requested) {
        s_stop_requested = false;
        return true;
    }
    return false;
}

/* Python 侧读取：优先转发队列，其次驱动回调（兜底） */
static bool xm_btn_read(xm_btn_event_t *evt)
{
    if (!evt) return false;
    xm_btn_queue_ensure();
    /* 先读转发队列（main 循环转发来的），再读驱动回调 */
    if (s_py_btn_queue && xQueueReceive(s_py_btn_queue, evt, 0) == pdTRUE) {
        return true;
    }
    /* 驱动回调直读（兜底：main 循环未消费时仍能响应） */
    if (s_btn_read_cb) {
        return s_btn_read_cb(evt);
    }
    return false;
}

/* 标记 Python 按键消费者活跃（main 循环据此转发按键） */
static void xm_btn_mark_active(void)
{
    if (!s_py_btn_active) {
        s_py_btn_active = true;
    }
}

/* ========== 颜色转换：0xRRGGBB -> RGB565 (SWAPPED) ========== */
static uint16_t xm_rgb565(uint32_t rgb)
{
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    /* RGB565 SWAPPED 字节序：低字节在前 */
    return (uint16_t)((c >> 8) | (c << 8));
}

/* ========== 帧缓冲管理 ========== */
bool xiaomiao_display_init(void)
{
    if (s_fb) return true;
    s_fb = heap_caps_malloc(XM_SCREEN_W * XM_SCREEN_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = malloc(XM_SCREEN_W * XM_SCREEN_H * 2);
    }
    if (!s_fb) return false;
    memset(s_fb, 0, XM_SCREEN_W * XM_SCREEN_H * 2);
    return true;
}

void xiaomiao_display_set_flush_cb(xm_flush_fn cb)
{
    s_flush_cb = cb;
}

uint16_t *xiaomiao_display_get_framebuffer(void)
{
    return s_fb;
}

/* ========== MicroPython 方法实现 ========== */

/* xiaomiao.init() -> None：初始化帧缓冲（幂等） */
static mp_obj_t xm_init(void)
{
    if (!xiaomiao_display_init()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("framebuffer alloc failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(xm_init_obj, xm_init);

/* xiaomiao.fill(color) -> None：清屏为指定颜色 */
static mp_obj_t xm_fill(mp_obj_t color_obj)
{
    if (!xiaomiao_display_init()) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    uint32_t rgb = mp_obj_get_int(color_obj) & 0xFFFFFF;
    uint16_t c = xm_rgb565(rgb);
    for (int i = 0; i < XM_SCREEN_W * XM_SCREEN_H; i++) s_fb[i] = c;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xm_fill_obj, xm_fill);

/* xiaomiao.pixel(x, y, color) -> None：画点 */
static mp_obj_t xm_pixel(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t color_obj)
{
    if (!xiaomiao_display_init()) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    int x = mp_obj_get_int(x_obj);
    int y = mp_obj_get_int(y_obj);
    if (x < 0 || x >= XM_SCREEN_W || y < 0 || y >= XM_SCREEN_H) return mp_const_none;
    s_fb[y * XM_SCREEN_W + x] = xm_rgb565(mp_obj_get_int(color_obj) & 0xFFFFFF);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(xm_pixel_obj, xm_pixel);

/* xiaomiao.rect(x, y, w, h, color, fill) -> None：矩形 */
static mp_obj_t xm_rect(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t w_obj, mp_obj_t h_obj,
                        mp_obj_t color_obj, mp_obj_t fill_obj)
{
    if (!xiaomiao_display_init()) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    int x = mp_obj_get_int(x_obj), y = mp_obj_get_int(y_obj);
    int w = mp_obj_get_int(w_obj), h = mp_obj_get_int(h_obj);
    uint16_t c = xm_rgb565(mp_obj_get_int(color_obj) & 0xFFFFFF);
    bool fill = mp_obj_is_true(fill_obj);

    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= XM_SCREEN_W || j < 0 || j >= XM_SCREEN_H) continue;
            bool on_edge = (i == x || i == x + w - 1 || j == y || j == y + h - 1);
            if (fill || on_edge) s_fb[j * XM_SCREEN_W + i] = c;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_6(xm_rect_obj, xm_rect);

/* xiaomiao.line(x0, y0, x1, y1, color) -> None：Bresenham直线 */
static mp_obj_t xm_line(mp_obj_t x0o, mp_obj_t y0o, mp_obj_t x1o, mp_obj_t y1o, mp_obj_t color_obj)
{
    if (!xiaomiao_display_init()) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    int x0 = mp_obj_get_int(x0o), y0 = mp_obj_get_int(y0o);
    int x1 = mp_obj_get_int(x1o), y1 = mp_obj_get_int(y1o);
    uint16_t c = xm_rgb565(mp_obj_get_int(color_obj) & 0xFFFFFF);

    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        if (x0 >= 0 && x0 < XM_SCREEN_W && y0 >= 0 && y0 < XM_SCREEN_H)
            s_fb[y0 * XM_SCREEN_W + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_5(xm_line_obj, xm_line);

/* xiaomiao.rect_fill(x, y, w, h, color) -> None：填充矩形（便捷） */
static mp_obj_t xm_rect_fill(mp_obj_t x_obj, mp_obj_t y_obj, mp_obj_t w_obj, mp_obj_t h_obj,
                             mp_obj_t color_obj)
{
    if (!xiaomiao_display_init()) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("no framebuffer"));
    int x = mp_obj_get_int(x_obj), y = mp_obj_get_int(y_obj);
    int w = mp_obj_get_int(w_obj), h = mp_obj_get_int(h_obj);
    uint16_t c = xm_rgb565(mp_obj_get_int(color_obj) & 0xFFFFFF);
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= XM_SCREEN_H) continue;
        for (int i = x; i < x + w; i++) {
            if (i >= 0 && i < XM_SCREEN_W) s_fb[j * XM_SCREEN_W + i] = c;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_5(xm_rect_fill_obj, xm_rect_fill);

/* xiaomiao.show() -> None：将 framebuffer 上屏 */
static mp_obj_t xm_show(void)
{
    if (s_flush_cb) s_flush_cb();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(xm_show_obj, xm_show);

/* xiaomiao.get_key() -> int：非阻塞读按键（-1=无） */
static mp_obj_t xm_get_key(void)
{
    xm_btn_event_t evt;
    xm_btn_mark_active();  /* 标记 Python 按键消费者活跃 */
    /* 协作式停止：B键返回时置位，此处抛出 KeyboardInterrupt 终止脚本 */
    if (xm_stop_check()) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    if (xm_btn_read(&evt)) {
        return mp_obj_new_int(evt.key);
    }
    return mp_obj_new_int(XM_KEY_NONE);
}
static MP_DEFINE_CONST_FUN_OBJ_0(xm_get_key_obj, xm_get_key);

/* xiaomiao.millis() -> int：毫秒时间戳 */
static mp_obj_t xm_millis(void)
{
    return mp_obj_new_int((mp_int_t)(esp_timer_get_time() / 1000));
}
static MP_DEFINE_CONST_FUN_OBJ_0(xm_millis_obj, xm_millis);

/* xiaomiao.sleep_ms(ms) -> None：睡眠（任务让步） */
static mp_obj_t xm_sleep_ms(mp_obj_t ms_obj)
{
    int ms = mp_obj_get_int(ms_obj);
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(xm_sleep_ms_obj, xm_sleep_ms);

/* xiaomiao.width() / xiaomiao.height() */
static mp_obj_t xm_width(void) { return mp_obj_new_int(XM_SCREEN_W); }
static MP_DEFINE_CONST_FUN_OBJ_0(xm_width_obj, xm_width);
static mp_obj_t xm_height(void) { return mp_obj_new_int(XM_SCREEN_H); }
static MP_DEFINE_CONST_FUN_OBJ_0(xm_height_obj, xm_height);

/* ========== 模块全局表 ========== */
static const mp_rom_map_elem_t xiaomiao_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_xiaomiao) },
    { MP_ROM_QSTR(MP_QSTR_init),      MP_ROM_PTR(&xm_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill),      MP_ROM_PTR(&xm_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel),     MP_ROM_PTR(&xm_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect),      MP_ROM_PTR(&xm_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_line),      MP_ROM_PTR(&xm_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect_fill), MP_ROM_PTR(&xm_rect_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_show),      MP_ROM_PTR(&xm_show_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_key),   MP_ROM_PTR(&xm_get_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_millis),    MP_ROM_PTR(&xm_millis_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep_ms),  MP_ROM_PTR(&xm_sleep_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_width),     MP_ROM_PTR(&xm_width_obj) },
    { MP_ROM_QSTR(MP_QSTR_height),    MP_ROM_PTR(&xm_height_obj) },
    /* 按键常量 */
    { MP_ROM_QSTR(MP_QSTR_KEY_UP),    MP_ROM_INT(XM_KEY_UP) },
    { MP_ROM_QSTR(MP_QSTR_KEY_DOWN),  MP_ROM_INT(XM_KEY_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_KEY_LEFT),  MP_ROM_INT(XM_KEY_LEFT) },
    { MP_ROM_QSTR(MP_QSTR_KEY_RIGHT), MP_ROM_INT(XM_KEY_RIGHT) },
    { MP_ROM_QSTR(MP_QSTR_KEY_A),     MP_ROM_INT(XM_KEY_A) },
    { MP_ROM_QSTR(MP_QSTR_KEY_B),     MP_ROM_INT(XM_KEY_B) },
};
static MP_DEFINE_CONST_DICT(xiaomiao_module_globals, xiaomiao_module_globals_table);

const mp_obj_module_t xiaomiao_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&xiaomiao_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_xiaomiao, xiaomiao_module);