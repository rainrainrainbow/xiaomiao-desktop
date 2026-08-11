/*
 * Xiaomiao LCD Test Firmware - v39
 *
 * 完全照抄 xiaomiao-loader 的 ST7735 初始化时序（这是已验证可用的版本）
 * 不依赖 LVGL，只做 LCD 硬件测试。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "return_to_loader.h"

/* ═══════════════════════════════════════════════════════════════════════
 * 硬件引脚定义 (照抄 xiaomiao-loader)
 * ═══════════════════════════════════════════════════════════════════════*/
#define LCD_HOST                    SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ          (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES            128
#define LCD_NATIVE_V_RES            160
#define LCD_H_RES                   160
#define LCD_V_RES                   128
#define LCD_DRAW_BUF_LINES          LCD_V_RES

#define PIN_LCD_SCLK                GPIO_NUM_18
#define PIN_LCD_MOSI                GPIO_NUM_23
#define PIN_LCD_MISO                GPIO_NUM_19
#define PIN_LCD_CS                  GPIO_NUM_5
#define PIN_LCD_DC                  GPIO_NUM_4
#define PIN_LCD_BL                  GPIO_NUM_0

#define LCD_BL_LEDC_TIMER           LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL         LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ         5000
#define LCD_BL_LEDC_RES             LEDC_TIMER_13_BIT

/* ST7735 寄存器 */
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_DISPOFF  0x28
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

#define MADCTL_MX       0x40
#define MADCTL_MY       0x80
#define MADCTL_MV       0x20
#define MADCTL_RGB      0x00

static const char *TAG = "lcd_test";

/* ═══════════════════════════════════════════════════════════════════════
 * 背光控制
 * ═══════════════════════════════════════════════════════════════════════*/
static void backlight_init(uint8_t percent)
{
    ledc_timer_config_t t = {
        .duty_resolution = LCD_BL_LEDC_RES,
        .freq_hz = LCD_BL_LEDC_FREQ_HZ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .channel = LCD_BL_LEDC_CHANNEL,
        .duty = 0,
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LCD_BL_LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));

    uint32_t max_duty = (1U << LCD_BL_LEDC_RES) - 1;
    uint32_t duty = (max_duty * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL));

    ESP_LOGI(TAG, "Backlight initialized at %d%%", percent);
}

/* ═══════════════════════════════════════════════════════════════════════
 * ST7735 初始化 - 完全照抄 xiaomiao-loader 的 st7735_init_black_tab_rot90
 * ═══════════════════════════════════════════════════════════════════════*/
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;

static void st7735_tx_param(esp_lcd_panel_io_handle_t io, int cmd,
                            const void *param, size_t param_size)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, cmd, param, param_size));
}

static void st7735_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void st7735_clear_black(esp_lcd_panel_io_handle_t io_handle)
{
    /* 静态大缓冲（避免栈溢出） */
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    memset(line, 0, sizeof(line));
    st7735_tx_param(io_handle, ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        st7735_tx_param(io_handle, ST7735_RASET, raset, sizeof(raset));
        st7735_tx_param(io_handle, ST7735_RAMWR, line,
                        (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
}

static void st7735_init_black_tab_rot90(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[]  = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[]  = {0x07};
    const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A, 0x00};
    const uint8_t pwctr4[]  = {0x8A, 0x2A};
    const uint8_t pwctr5[]  = {0x8A, 0xEE};
    const uint8_t vmctr1[]  = {0x0E};
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[]   = {0x05};
    const uint8_t caset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gp[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                          0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gn[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                          0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    /* === 照抄 loader 的时序 === */
    st7735_tx_param(io, ST7735_DISPOFF, NULL, 0);
    st7735_tx_param(io, ST7735_SWRESET, NULL, 0);
    /* 注意：loader 没有 150ms delay！ */
    st7735_tx_param(io, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st7735_tx_param(io, ST7735_VMCTR1, vmctr1, sizeof(vmctr1));
    st7735_tx_param(io, ST7735_INVOFF, NULL, 0);
    st7735_tx_param(io, ST7735_MADCTL, madctl_d, sizeof(madctl_d));
    st7735_tx_param(io, ST7735_COLMOD, colmod, sizeof(colmod));
    st7735_tx_param(io, ST7735_CASET, caset, sizeof(caset));
    st7735_tx_param(io, ST7735_RASET, raset, sizeof(raset));
    st7735_tx_param(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st7735_tx_param(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st7735_tx_param(io, ST7735_NORON, NULL, 0);
    st7735_delay_ms(10);
    st7735_tx_param(io, ST7735_MADCTL, madctl_r, sizeof(madctl_r));
    st7735_clear_black(io);

    ESP_LOGI(TAG, "ST7735 init complete (black tab, rot 90)");
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    ESP_LOGI(TAG, "Init SPI bus + ST7735 TFT");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
                        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));
    s_lcd_io = io;
    st7735_init_black_tab_rot90(io);
    return io;
}

/* ═══════════════════════════════════════════════════════════════════════
 * LCD 绘图函数 - 简化版，使用 tx_color 而不是 LVGL
 * ═══════════════════════════════════════════════════════════════════════*/
static void lcd_set_window(esp_lcd_panel_io_handle_t io, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    const uint8_t caset[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    const uint8_t raset[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    esp_lcd_panel_io_tx_param(io, ST7735_CASET, caset, sizeof(caset));
    esp_lcd_panel_io_tx_param(io, ST7735_RASET, raset, sizeof(raset));
}

/* 填充纯色 */
static void lcd_fill_color(esp_lcd_panel_io_handle_t io, uint16_t color)
{
    lcd_set_window(io, 0, 0, LCD_H_RES-1, LCD_V_RES-1);
    /* 单行缓冲 - 用 DMA 缓冲以提高速度 */
    static uint16_t line[LCD_H_RES];
    for (int i = 0; i < LCD_H_RES; i++) line[i] = color;

    /* 按行发送 */
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, line, LCD_H_RES * sizeof(uint16_t));
    }

    ESP_LOGI(TAG, "Filled with color 0x%04X", color);
}

/* 画 4 块色彩 */
static void lcd_test_4colors(esp_lcd_panel_io_handle_t io)
{
    const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF}; /* R G B W */

    for (int q = 0; q < 4; q++) {
        int x0 = (q % 2) * (LCD_H_RES / 2);
        int y0 = (q / 2) * (LCD_V_RES / 2);
        int x1 = x0 + LCD_H_RES/2 - 1;
        int y1 = y0 + LCD_V_RES/2 - 1;

        lcd_set_window(io, x0, y0, x1, y1);

        static uint16_t line[LCD_H_RES/2];
        for (int i = 0; i < LCD_H_RES/2; i++) line[i] = colors[q];

        for (int y = y0; y <= y1; y++) {
            esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, line, (LCD_H_RES/2) * sizeof(uint16_t));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 主函数
 * ═══════════════════════════════════════════════════════════════════════*/
void app_main(void)
{
    return_to_loader_setup();
    ESP_LOGI(TAG, "========== Xiaomiao LCD Test v39 (loader timing) ==========");

    /* 1. 初始化背光 - 100% */
    backlight_init(100);

    /* 2. 初始化 SPI + LCD (使用 loader 的时序) */
    esp_lcd_panel_io_handle_t io = lcd_init();

    /* 3. 打开显示 */
    st7735_tx_param(s_lcd_io, ST7735_DISPON, NULL, 0);
    st7735_delay_ms(20);

    /* 4. 循环测试 5 种颜色 */
    int test_idx = 0;
    while (true) {
        switch (test_idx % 5) {
            case 0:
                ESP_LOGI(TAG, "[TEST %d] Full WHITE", test_idx);
                lcd_fill_color(io, 0xFFFF);
                break;
            case 1:
                ESP_LOGI(TAG, "[TEST %d] Full RED", test_idx);
                lcd_fill_color(io, 0xF800);
                break;
            case 2:
                ESP_LOGI(TAG, "[TEST %d] Full GREEN", test_idx);
                lcd_fill_color(io, 0x07E0);
                break;
            case 3:
                ESP_LOGI(TAG, "[TEST %d] Full BLUE", test_idx);
                lcd_fill_color(io, 0x001F);
                break;
            case 4:
                ESP_LOGI(TAG, "[TEST %d] 4-color quadrants", test_idx);
                lcd_test_4colors(io);
                break;
        }
        test_idx++;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}