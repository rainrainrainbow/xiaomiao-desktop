/**
 * @file ion/display.h
 * @brief Ion - Hardware Abstraction Layer: Display Interface
 * 
 * 参考 NumWorks Epsilon 的 Ion 层设计，提供统一的显示接口。
 * 底层实现可以是 ST7735、ILI9341 或其他 TFT 驱动。
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

/* 显示分辨率（横屏） */
#define ION_DISPLAY_WIDTH  160
#define ION_DISPLAY_HEIGHT 128

/* 颜色格式：RGB565 */
typedef uint16_t ion_color_t;

/* 颜色常量 */
#define ION_COLOR_BLACK   0x0000
#define ION_COLOR_WHITE   0xFFFF
#define ION_COLOR_RED     0xF800
#define ION_COLOR_GREEN   0x07E0
#define ION_COLOR_BLUE    0x001F

/**
 * @brief 初始化显示驱动
 * @return true 成功，false 失败
 */
bool ion_display_init(void);

/**
 * @brief 设置背光亮度
 * @param brightness 亮度值（0-100）
 */
void ion_display_set_brightness(uint8_t brightness);

/**
 * @brief 获取当前背光亮度
 * @return 亮度值（0-100）
 */
uint8_t ion_display_get_brightness(void);

/**
 * @brief 填充整个屏幕为指定颜色
 * @param color RGB565 颜色值
 */
void ion_display_fill(ion_color_t color);

/**
 * @brief 在指定位置绘制单个像素
 * @param x X 坐标（0-159）
 * @param y Y 坐标（0-127）
 * @param color RGB565 颜色值
 */
void ion_display_draw_pixel(int x, int y, ion_color_t color);

/**
 * @brief 批量绘制像素（用于快速刷新）
 * @param x 起始 X 坐标
 * @param y 起始 Y 坐标
 * @param width 宽度
 * @param height 高度
 * @param pixels 像素数据数组（RGB565 格式）
 */
void ion_display_draw_pixels(int x, int y, int width, int height, const ion_color_t *pixels);

/**
 * @brief 获取帧缓冲区指针（如果支持双缓冲）
 * @return 帧缓冲区指针，NULL 表示不支持
 */
ion_color_t* ion_display_get_framebuffer(void);

/**
 * @brief 刷新显示（将帧缓冲区内容发送到屏幕）
 */
void ion_display_flush(void);

#endif /* DISPLAY_H */
