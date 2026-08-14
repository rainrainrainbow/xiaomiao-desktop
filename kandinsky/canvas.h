/**
 * @file kandinsky/canvas.h
 * @brief Kandinsky - Graphics Engine: Canvas Interface
 * 
 * 参考 NumWorks Epsilon 的 Kandinsky 层设计，提供统一的画布操作接口。
 * 基于 Ion 层的显示驱动，提供高级绘图功能。
 */

#ifndef KANDINSKY_CANVAS_H
#define KANDINSKY_CANVAS_H

#include <stdint.h>
#include <stdbool.h>
#include "ion/display.h"

/* 画布结构体 */
typedef struct {
    int width;
    int height;
    ion_color_t *pixels;  /* 像素数据 */
    bool owns_buffer;     /* 是否拥有缓冲区所有权 */
} kandinsky_canvas_t;

/**
 * @brief 创建画布
 * @param width 宽度
 * @param height 高度
 * @return 画布指针，NULL 表示失败
 */
kandinsky_canvas_t* kandinsky_canvas_create(int width, int height);

/**
 * @brief 销毁画布
 * @param canvas 画布指针
 */
void kandinsky_canvas_destroy(kandinsky_canvas_t *canvas);

/**
 * @brief 填充画布为指定颜色
 * @param canvas 画布指针
 * @param color RGB565 颜色值
 */
void kandinsky_canvas_fill(kandinsky_canvas_t *canvas, ion_color_t color);

/**
 * @brief 绘制单个像素
 * @param canvas 画布指针
 * @param x X 坐标
 * @param y Y 坐标
 * @param color 颜色值
 */
void kandinsky_canvas_draw_pixel(kandinsky_canvas_t *canvas, int x, int y, ion_color_t color);

/**
 * @brief 绘制直线
 * @param canvas 画布指针
 * @param x0 起点 X
 * @param y0 起点 Y
 * @param x1 终点 X
 * @param y1 终点 Y
 * @param color 颜色值
 */
void kandinsky_canvas_draw_line(kandinsky_canvas_t *canvas, int x0, int y0, int x1, int y1, ion_color_t color);

/**
 * @brief 绘制矩形（边框）
 * @param canvas 画布指针
 * @param x 左上角 X
 * @param y 左上角 Y
 * @param width 宽度
 * @param height 高度
 * @param color 颜色值
 */
void kandinsky_canvas_draw_rect(kandinsky_canvas_t *canvas, int x, int y, int width, int height, ion_color_t color);

/**
 * @brief 填充矩形
 * @param canvas 画布指针
 * @param x 左上角 X
 * @param y 左上角 Y
 * @param width 宽度
 * @param height 高度
 * @param color 颜色值
 */
void kandinsky_canvas_fill_rect(kandinsky_canvas_t *canvas, int x, int y, int width, int height, ion_color_t color);

/**
 * @brief 绘制圆（边框）
 * @param canvas 画布指针
 * @param cx 圆心 X
 * @param cy 圆心 Y
 * @param radius 半径
 * @param color 颜色值
 */
void kandinsky_canvas_draw_circle(kandinsky_canvas_t *canvas, int cx, int cy, int radius, ion_color_t color);

/**
 * @brief 填充圆
 * @param canvas 画布指针
 * @param cx 圆心 X
 * @param cy 圆心 Y
 * @param radius 半径
 * @param color 颜色值
 */
void kandinsky_canvas_fill_circle(kandinsky_canvas_t *canvas, int cx, int cy, int radius, ion_color_t color);

/**
 * @brief 将画布内容刷新到屏幕
 * @param canvas 画布指针
 * @param dst_x 目标屏幕 X 坐标
 * @param dst_y 目标屏幕 Y 坐标
 */
void kandinsky_canvas_flush(kandinsky_canvas_t *canvas, int dst_x, int dst_y);

#endif /* KANDINSKY_CANVAS_H */