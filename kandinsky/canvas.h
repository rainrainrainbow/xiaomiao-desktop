/**
 * @file kandinsky/canvas.h
 * @brief Kandinsky - Graphics Engine: Canvas Interface
 * 
 * 参考 NumWorks Epsilon 的 Kandinsky 层设计，提供统一的画布操作接口。
 * 基于全局帧缓冲区，提供高级绘图功能。
 * 坐标系：左上角原点，X 向右，Y 向下。
 * 颜色格式：RGB565。
 */

#ifndef KANDINSKY_CANVAS_H
#define KANDINSKY_CANVAS_H

#include <stdint.h>
#include <stdbool.h>
#include "ion/display.h"

/* ========== 颜色常量 ========== */
#define KD_COLOR_BLACK       ION_COLOR_BLACK
#define KD_COLOR_WHITE       ION_COLOR_WHITE
#define KD_COLOR_RED         ION_COLOR_RED
#define KD_COLOR_GREEN       ION_COLOR_GREEN
#define KD_COLOR_BLUE        ION_COLOR_BLUE
#define KD_COLOR_FROM_RGB(r, g, b) \
    ((ion_color_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/**
 * @brief 初始化画布（绑定到帧缓冲区）
 * @param framebuffer 帧缓冲区指针（PSRAM 或 DRAM）
 */
void kd_canvas_init(ion_color_t *framebuffer);

/**
 * @brief 清空画布为指定颜色
 * @param color RGB565 颜色值
 */
void kd_canvas_clear(ion_color_t color);

/**
 * @brief 设置裁剪区域
 * @param x 左上角 X
 * @param y 左上角 Y
 * @param w 宽度
 * @param h 高度
 */
void kd_canvas_set_clip(int x, int y, int w, int h);

/**
 * @brief 重置裁剪区域为全屏
 */
void kd_canvas_reset_clip(void);

/**
 * @brief 绘制单个像素
 * @param x X 坐标
 * @param y Y 坐标
 * @param color RGB565 颜色值
 */
void kd_canvas_set_pixel(int x, int y, ion_color_t color);

/**
 * @brief 读取像素颜色
 * @param x X 坐标
 * @param y Y 坐标
 * @return 像素颜色值
 */
ion_color_t kd_canvas_get_pixel(int x, int y);

/**
 * @brief 绘制直线（Bresenham 算法）
 */
void kd_canvas_draw_line(int x0, int y0, int x1, int y1, ion_color_t color);

/**
 * @brief 绘制矩形边框
 */
void kd_canvas_draw_rect(int x, int y, int w, int h, ion_color_t color);

/**
 * @brief 填充矩形
 */
void kd_canvas_fill_rect(int x, int y, int w, int h, ion_color_t color);

/**
 * @brief 填充圆角矩形
 * @param r 圆角半径
 */
void kd_canvas_fill_round_rect(int x, int y, int w, int h, int r, ion_color_t color);

/**
 * @brief 绘制圆边框（Bresenham 算法）
 */
void kd_canvas_draw_circle(int cx, int cy, int r, ion_color_t color);

/**
 * @brief 填充圆形
 */
void kd_canvas_fill_circle(int cx, int cy, int r, ion_color_t color);

/**
 * @brief 绘制位图（跳过 0x0000 透明色）
 */
void kd_canvas_draw_bitmap(int x, int y, int w, int h, const ion_color_t *data);

/**
 * @brief 缩放绘制位图
 */
void kd_canvas_draw_bitmap_scaled(int x, int y, int dw, int dh,
                                   const ion_color_t *src, int sw, int sh);

/**
 * @brief 从 RGB 分量合成 RGB565 颜色
 */
ion_color_t kd_color_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 将 RGB565 颜色拆分为 RGB 分量
 */
void kd_color_split(ion_color_t color, uint8_t *r, uint8_t *g, uint8_t *b);

/**
 * @brief 颜色混合（Alpha 混合）
 * @param fg 前景色
 * @param bg 背景色
 * @param alpha 透明度（0-255，0=全透明，255=全不透明）
 */
ion_color_t kd_color_blend(ion_color_t fg, ion_color_t bg, uint8_t alpha);

#endif /* KANDINSKY_CANVAS_H */