/**
 * @file kandinsky/font.h
 * @brief Kandinsky - Graphics Engine: Font Interface
 * 
 * 参考 NumWorks Epsilon 的 Kandinsky 层设计，提供统一的字体渲染接口。
 * 内置 5x7 ASCII 点阵字体，支持自定义外部字体桥接。
 */

#ifndef KANDINSKY_FONT_H
#define KANDINSKY_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "ion/display.h"

/* 字体类型枚举 */
typedef enum {
    KD_FONT_5X7 = 0,      /* 内置 5x7 点阵字体 */
    KD_FONT_EXTERNAL,      /* 外部字体（如 LVGL 字体） */
} kd_font_type_t;

/* 字体结构体 */
typedef struct kd_font_s {
    kd_font_type_t type;                          /* 字体类型 */
    int width;                                    /* 字符宽度（像素） */
    int height;                                   /* 字符高度（像素） */
    void (*render_char)(int x, int y, char c,     /* 渲染回调（外部字体） */
                        ion_color_t fg, ion_color_t bg);
} kd_font_t;

/* 预定义 5x7 字体 */
extern const kd_font_t kd_font_5x7;

/**
 * @brief 获取字体高度
 * @param font 字体指针，NULL 使用默认 5x7
 * @return 字体高度（像素）
 */
int kd_font_height(const kd_font_t *font);

/**
 * @brief 获取字符宽度
 * @param font 字体指针
 * @param c ASCII 字符
 * @return 字符宽度（像素）
 */
int kd_font_char_width(const kd_font_t *font, char c);

/**
 * @brief 计算字符串宽度
 * @param font 字体指针
 * @param str 字符串
 * @return 字符串总宽度（像素）
 */
int kd_font_string_width(const kd_font_t *font, const char *str);

/**
 * @brief 渲染单个字符
 * @param x, y 绘制位置
 * @param font 字体指针
 * @param c 要渲染的字符
 * @param fg 前景色
 * @param bg 背景色（0xFFFF 表示透明）
 */
void kd_font_draw_char(int x, int y, const kd_font_t *font, char c,
                        ion_color_t fg, ion_color_t bg);

/**
 * @brief 渲染字符串
 * @param x, y 绘制起始位置
 * @param font 字体指针
 * @param str 字符串（支持 \n 换行）
 * @param fg 前景色
 * @param bg 背景色（0xFFFF 表示透明）
 */
void kd_font_draw_string(int x, int y, const kd_font_t *font,
                          const char *str, ion_color_t fg, ion_color_t bg);

/**
 * @brief 限制宽度内渲染字符串（自动换行）
 * @param x, y 绘制起始位置
 * @param font 字体指针
 * @param str 字符串
 * @param max_width 最大宽度（像素）
 * @param fg 前景色
 * @param bg 背景色
 */
void kd_font_draw_string_wrap(int x, int y, const kd_font_t *font,
                               const char *str, int max_width,
                               ion_color_t fg, ion_color_t bg);

/**
 * @brief 设置默认字体
 * @param font 字体指针
 */
void kd_font_set_default(const kd_font_t *font);

/**
 * @brief 获取默认字体
 * @return 默认字体指针
 */
const kd_font_t *kd_font_get_default(void);

#endif /* KANDINSKY_FONT_H */