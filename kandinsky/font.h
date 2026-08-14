/**
 * @file kandinsky/font.h
 * @brief Kandinsky - Graphics Engine: Font Interface
 * 
 * 参考 NumWorks Epsilon 的 Kandinsky 层设计，提供统一的字体渲染接口。
 * 支持 CJK 中文字库和 FontAwesome 图标。
 */

#ifndef KANDINSKY_FONT_H
#define KANDINSKY_FONT_H

#include <stdint.h>
#include <stdbool.h>
#include "ion/display.h"

/* 字体结构体（前向声明） */
typedef struct lv_font_t lv_font_t;

/**
 * @brief 初始化字体引擎
 * @return true 成功，false 失败
 */
bool kandinsky_font_init(void);

/**
 * @brief 获取指定大小的中文字体
 * @param size 字体大小（14 或 16）
 * @return 字体指针，NULL 表示失败
 */
const lv_font_t* kandinsky_font_get_cn(uint8_t size);

/**
 * @brief 获取 FontAwesome 图标字体
 * @return 字体指针
 */
const lv_font_t* kandinsky_font_get_icon(void);

/**
 * @brief 测量文本宽度
 * @param font 字体指针
 * @param text UTF-8 编码的文本
 * @return 文本宽度（像素）
 */
int kandinsky_font_measure_text(const lv_font_t *font, const char *text);

/**
 * @brief 测量文本高度
 * @param font 字体指针
 * @return 文本高度（像素）
 */
int kandinsky_font_measure_height(const lv_font_t *font);

#endif /* KANDINSKY_FONT_H */