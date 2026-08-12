/*
 * Xiaomiao LCD Test Firmware - v41
 *
 * 完全照抄 xiaomiao-loader 的 ST7735 初始化时序（st7735_init_black_tab_rot90），
 * 使用底层 spi_device 直接驱动，跳过 esp_lcd_panel_io 抽象层。
 * 关键修复：
 *  1. 补上 SLPOUT + 500ms 延迟（v40 遗漏）
 *  2. 补上 FRMCTR1/2/3、INVCTR、PWCTR1-4（v40 遗漏）
 *  3. 补上 clear_black（按 8 行分块发送，避免大块 DMA 传输问题）
 *  4. 使用与 loader 完全一致的寄存器顺序
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
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "return_to_loader.h"

/* ═══════════════════════════════════════════════════════════════════════
 * 硬件引脚（与 xiaomiao-loader 完全一致）
 * ═══════════════════════════════════════════════════════════════════════*/
#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160
#define LCD_H_RES           160
#define LCD_V_RES           128
#define LCD_DRAW_BUF_LINES  LCD_V_RES
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8

#define PIN_LCD_SCLK        GPIO_NUM_18
#define PIN_LCD_MOSI        GPIO_NUM_23
#define PIN_LCD_MISO        GPIO_NUM_19
#define PIN_LCD_CS          GPIO_NUM_5
#define PIN_LCD_DC          GPIO_NUM_4
#define PIN_LCD_BL          GPIO_NUM_0

#define LCD_X_GAP           0
#define LCD_Y_GAP           0

#define LCD_BL_LEDC_TIMER   LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ 5000
#define LCD_BL_LEDC_RES     LEDC_TIMER_13_BIT

/* ST7735 寄存器（与 loader 完全一致） */
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
static spi_device_handle_t s_spi_dev;

/* ═══════════════════════════════════════════════════════════════════════
 * 背光
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
        .channel = LCD_BL_LEDC_CHANNEL, .duty = 0,
        .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LCD_BL_LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    uint32_t max_duty = (1U << LCD_BL_LEDC_RES) - 1;
    uint32_t duty = (max_duty * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL));
    ESP_LOGI(TAG, "Backlight %d%%", percent);
}

/* ═══════════════════════════════════════════════════════════════════════
 * 底层 SPI 发送（DC 手动控制，与 loader 的 esp_lcd_panel_io 行为一致）
 * ═══════════════════════════════════════════════════════════════════════*/
static void lcd_cmd(uint8_t cmd)
{
    gpio_set_level(PIN_LCD_DC, 0);   /* DC=0: command */
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

static void lcd_data(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_LCD_DC, 1);   /* DC=1: data */
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    lcd_cmd(cmd);
    if (len) lcd_data(data, len);
}

static void lcd_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* ═══════════════════════════════════════════════════════════════════════
 * 清屏 — 照抄 loader 的 st7735_clear_black()
 * 按 8 行分块发送，避免一次性发送整屏导致 DMA 问题
 * ═══════════════════════════════════════════════════════════════════════*/
static void st7735_clear_black(void)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};  /* 160 */
    memset(line, 0, sizeof(line));
    lcd_cmd_data(ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        lcd_cmd_data(ST7735_RASET, raset, sizeof(raset));
        lcd_cmd(ST7735_RAMWR);
        lcd_data((uint8_t*)line, (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * ST7735 初始化 — 精确照抄 loader 的 st7735_init_black_tab_rot90()
 * ═══════════════════════════════════════════════════════════════════════*/
static void st7735_init(void)
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
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};  /* 0xC0 */
    const uint8_t colmod[]   = {0x05};
    const uint8_t caset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};  /* 128 */
    const uint8_t raset[]    = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};  /* 160 */
    const uint8_t gp[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    const uint8_t gn[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};  /* 0x60 */

    /* 与 loader 完全一致的顺序 */
    lcd_cmd(ST7735_DISPOFF);
    lcd_cmd(ST7735_SWRESET);
    lcd_delay(150);
    lcd_cmd(ST7735_SLPOUT);
    lcd_delay(500);
    lcd_cmd_data(ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    lcd_cmd_data(ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    lcd_cmd_data(ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    lcd_cmd_data(ST7735_INVCTR,  invctr, sizeof(invctr));
    lcd_cmd_data(ST7735_PWCTR1,  pwctr1, sizeof(pwctr1));
    lcd_cmd_data(ST7735_PWCTR2,  pwctr2, sizeof(pwctr2));
    lcd_cmd_data(ST7735_PWCTR3,  pwctr3, sizeof(pwctr3));
    lcd_cmd_data(ST7735_PWCTR4,  pwctr4, sizeof(pwctr4));
    lcd_cmd_data(ST7735_PWCTR5,  pwctr5, sizeof(pwctr5));
    lcd_cmd_data(ST7735_VMCTR1,  vmctr1, sizeof(vmctr1));
    lcd_cmd(ST7735_INVOFF);
    lcd_cmd_data(ST7735_MADCTL,  madctl_d, sizeof(madctl_d));
    lcd_cmd_data(ST7735_COLMOD,  colmod, sizeof(colmod));
    lcd_cmd_data(ST7735_CASET,   caset, sizeof(caset));
    lcd_cmd_data(ST7735_RASET,   raset, sizeof(raset));
    lcd_cmd_data(ST7735_GMCTRP1, gp, sizeof(gp));
    lcd_cmd_data(ST7735_GMCTRN1, gn, sizeof(gn));
    lcd_cmd(ST7735_NORON);
    lcd_delay(10);
    lcd_cmd_data(ST7735_MADCTL,  madctl_r, sizeof(madctl_r));

    /* 清屏（按 8 行分块，与 loader 一致） */
    st7735_clear_black();

    ESP_LOGI(TAG, "ST7735 init complete (loader timing, direct SPI)");
}

/* ═══════════════════════════════════════════════════════════════════════
 * 绘图
 * ═══════════════════════════════════════════════════════════════════════*/
static void lcd_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t caset[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    uint8_t raset[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    lcd_cmd_data(ST7735_CASET, caset, sizeof(caset));
    lcd_cmd_data(ST7735_RASET, raset, sizeof(raset));
}

/* 填充纯色 - 按 8 行分块发送 */
static void lcd_fill_color(uint16_t color)
{
    static uint16_t line[LCD_H_RES * 8];
    for (int i = 0; i < LCD_H_RES * 8; i++) line[i] = color;

    lcd_set_window(0, 0, LCD_H_RES-1, LCD_V_RES-1);
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        lcd_cmd_data(ST7735_RASET, raset, sizeof(raset));
        lcd_cmd(ST7735_RAMWR);
        lcd_data((uint8_t*)line, (y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
    ESP_LOGI(TAG, "Filled color 0x%04X", color);
}

/* 画 4 块色彩 */
static void lcd_test_4colors(void)
{
    const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    static uint16_t line[LCD_H_RES];
    for (int q = 0; q < 4; q++) {
        int x0 = (q % 2) * (LCD_H_RES / 2);
        int y0 = (q / 2) * (LCD_V_RES / 2);
        int x1 = x0 + LCD_H_RES/2 - 1;
        int y1 = y0 + LCD_V_RES/2 - 1;
        lcd_set_window(x0, y0, x1, y1);
        for (int i = 0; i < LCD_H_RES; i++) line[i] = colors[q];
        for (uint16_t yy = y0; yy <= y1; yy += 8) {
            const uint16_t yy2 = MIN((uint16_t)(yy + 7), (uint16_t)y1);
            const uint8_t raset[] = {yy >> 8, yy & 0xFF, yy2 >> 8, yy2 & 0xFF};
            lcd_cmd_data(ST7735_RASET, raset, sizeof(raset));
            lcd_cmd(ST7735_RAMWR);
            /* 每行 (x1-x0+1) 个像素 */
            lcd_data((uint8_t*)line, (x1 - x0 + 1) * (yy2 - yy + 1) * sizeof(uint16_t));
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * 主函数
 * ═══════════════════════════════════════════════════════════════════════*/
void app_main(void)
{
    return_to_loader_setup();
    ESP_LOGI(TAG, "========== Xiaomiao LCD Test v41 (loader timing + direct SPI) ==========");

    /* 1. 背光 */
    backlight_init(100);

    /* 2. 初始化 SPI 总线 */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 3. 添加 SPI 设备 */
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = LCD_PIXEL_CLOCK_HZ,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 10,
        .flags = SPI_DEVICE_HALFDUPLEX,  /* LCD 只发送不接收，半双工模式支持 60MHz */
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &devcfg, &s_spi_dev));

    /* 4. 初始化 DC 引脚 */
    gpio_set_direction(PIN_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_DC, 0);

    /* 5. 初始化 ST7735（loader 时序） */
    st7735_init();

    /* 6. 打开显示 */
    lcd_cmd(ST7735_DISPON);
    lcd_delay(20);

    /* 7. 循环测试 */
    int test_idx = 0;
    while (true) {
        switch (test_idx % 5) {
            case 0: lcd_fill_color(0xFFFF); break;  /* WHITE */
            case 1: lcd_fill_color(0xF800); break;  /* RED */
            case 2: lcd_fill_color(0x07E0); break;  /* GREEN */
            case 3: lcd_fill_color(0x001F); break;  /* BLUE */
            case 4: lcd_test_4colors(); break;      /* 4-color */
        }
        test_idx++;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}