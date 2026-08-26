/**
 * @file crash_screen.c
 * @brief 崩溃蓝屏显示模块
 * 
 * 当系统发生 panic 时，绕过 LVGL 直接在 LCD 上显示崩溃信息。
 * 类似 Windows 蓝屏，但适配 160x128 小屏幕。
 * 
 * 设计原则：
 * 1. 不依赖 LVGL（崩溃时 LVGL 可能已损坏）
 * 2. 不依赖堆内存（可能已耗尽）
 * 3. 使用静态缓冲区和底层 LCD 命令
 * 4. 显示关键调试信息：PC、EXCVADDR、调用栈
 */

#include "crash_screen.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CRASH";

// ST7735 命令（与 main.c 保持一致）
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36

// 屏幕尺寸（横屏）
#define LCD_W   160
#define LCD_H   128

// 颜色定义（RGB565 交换字节序）
#define COLOR_BLUE_BG     0xF800  // 蓝色背景（交换后）
#define COLOR_WHITE       0xFFFF
#define COLOR_YELLOW      0xFF00
#define COLOR_RED         0x001F

// 静态缓冲区（不使用堆内存）
static uint16_t s_crash_buf[LCD_W * 8];  // 8 行缓冲区

// 全局 LCD IO 句柄（由 main.c 设置）
static esp_lcd_panel_io_handle_t s_crash_lcd_io = NULL;

// 崩溃信息（由 panic handler 填充）
static crash_info_t s_crash_info;

/**
 * @brief 设置 LCD IO 句柄（由 main.c 在初始化后调用）
 */
void crash_screen_set_lcd_io(esp_lcd_panel_io_handle_t io)
{
    s_crash_lcd_io = io;
}

/**
 * @brief 发送 LCD 命令
 */
static void crash_lcd_cmd(uint8_t cmd, const void *param, size_t len)
{
    if (!s_crash_lcd_io) return;
    esp_lcd_panel_io_tx_param(s_crash_lcd_io, cmd, param, len);
}

/**
 * @brief 设置显示区域
 */
static void crash_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t caset[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    uint8_t raset[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    crash_lcd_cmd(ST7735_CASET, caset, sizeof(caset));
    crash_lcd_cmd(ST7735_RASET, raset, sizeof(raset));
}

/**
 * @brief 填充纯色矩形
 */
static void crash_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (!s_crash_lcd_io) return;
    
    // 填充缓冲区
    for (int i = 0; i < w * 8 && i < LCD_W * 8; i++) {
        s_crash_buf[i] = color;
    }
    
    crash_set_window(x, y, x + w - 1, y + h - 1);
    
    // 分块写入（每次 8 行）
    for (int row = 0; row < h; row += 8) {
        int rows = (row + 8 <= h) ? 8 : (h - row);
        crash_lcd_cmd(ST7735_RAMWR, s_crash_buf, w * rows * 2);
    }
}

/**
 * @brief 绘制简单字符（8x16 字体，仅支持 ASCII）
 * 
 * 使用最简化的 5x7 字体，放大到 8x16 显示
 */
static const uint8_t font_5x7[][5] = {
    // 空格到 ~ (可打印 ASCII)
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 空格
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x72, 0x49, 0x49, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x49, 0x4D, 0x33}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x41, 0x21, 0x11, 0x09, 0x07}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x26, 0x49, 0x49, 0x49, 0x3E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x41, 0x22, 0x14, 0x08, 0x00}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x41, 0x51, 0x72}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x26, 0x49, 0x49, 0x49, 0x32}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
};

/**
 * @brief 绘制单个字符（放大到 2 倍）
 */
static void crash_draw_char(uint16_t x, uint16_t y, char c, uint16_t color)
{
    if (c < ' ' || c > 'Z') c = '?';
    int idx = c - ' ';
    if (idx < 0 || idx >= sizeof(font_5x7) / sizeof(font_5x7[0])) return;
    
    const uint8_t *glyph = font_5x7[idx];
    
    // 逐像素绘制（2 倍放大）
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (glyph[col] & (1 << (6 - row))) {
                // 绘制 2x2 像素块
                crash_fill_rect(x + col * 2, y + row * 2, 2, 2, color);
            }
        }
    }
}

/**
 * @brief 绘制字符串
 */
static void crash_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t color)
{
    while (*str) {
        crash_draw_char(x, y, *str, color);
        x += 12;  // 字符宽度 5*2 + 2 间距
        str++;
        if (x > LCD_W - 12) break;  // 超出屏幕
    }
}

/**
 * @brief 显示崩溃蓝屏
 */
void crash_screen_show(const crash_info_t *info)
{
    if (!s_crash_lcd_io) {
        ESP_LOGE(TAG, "LCD IO not set, cannot show crash screen");
        return;
    }
    
    // 保存崩溃信息
    s_crash_info = *info;
    
    ESP_LOGE(TAG, "=== CRASH SCREEN ===");
    ESP_LOGE(TAG, "PC: 0x%08X", (unsigned int)info->pc);
    ESP_LOGE(TAG, "EXCVADDR: 0x%08X", (unsigned int)info->excvaddr);
    ESP_LOGE(TAG, "EXCCAUSE: 0x%02X", info->exccause);
    ESP_LOGE(TAG, "====================");
    
    // 1. 蓝色背景
    crash_fill_rect(0, 0, LCD_W, LCD_H, COLOR_BLUE_BG);
    
    // 2. 标题（黄色）
    crash_draw_string(20, 10, "SYSTEM CRASH", COLOR_YELLOW);
    
    // 3. 分隔线
    crash_fill_rect(10, 28, LCD_W - 20, 2, COLOR_WHITE);
    
    // 4. 崩溃信息（白色）
    char buf[32];
    int y = 36;
    
    snprintf(buf, sizeof(buf), "PC:%08X", (unsigned int)info->pc);
    crash_draw_string(10, y, buf, COLOR_WHITE);
    y += 16;
    
    snprintf(buf, sizeof(buf), "ADDR:%08X", (unsigned int)info->excvaddr);
    crash_draw_string(10, y, buf, COLOR_WHITE);
    y += 16;
    
    snprintf(buf, sizeof(buf), "CAUSE:%02X", info->exccause);
    crash_draw_string(10, y, buf, COLOR_WHITE);
    y += 16;
    
    // 5. 内存信息
    snprintf(buf, sizeof(buf), "DRAM:%luK", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    crash_draw_string(10, y, buf, COLOR_WHITE);
    y += 16;
    
    snprintf(buf, sizeof(buf), "PSRAM:%luK", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    crash_draw_string(10, y, buf, COLOR_WHITE);
    y += 20;
    
    // 6. 底部提示（红色）
    crash_draw_string(10, LCD_H - 20, "RESTARTING...", COLOR_RED);
}

/**
 * @brief 显示崩溃蓝屏并重启
 * 
 * 在检测到关键错误（如内存耗尽）时调用
 * 显示蓝屏信息后等待 5 秒重启
 * 
 * @param info 崩溃信息
 */
void crash_screen_show_and_restart(const crash_info_t *info)
{
    crash_screen_show(info);
    
    // 等待 5 秒让用户看到蓝屏
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // 重启系统
    ESP_LOGE(TAG, "System restarting...");
    esp_restart();
}
