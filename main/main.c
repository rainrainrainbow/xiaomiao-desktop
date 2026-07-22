/*
 * xiaomiao-desktop - MINIMAL TEST v2 (EXACTLY matching xiaomiao-loader)
 * Uses identical init sequence: clear_black BEFORE DISPON
 * Then draws test pattern.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "return_to_loader.h"

#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160
#define LCD_H_RES           160
#define LCD_V_RES           128

#define PIN_LCD_SCLK   GPIO_NUM_18
#define PIN_LCD_MOSI   GPIO_NUM_23
#define PIN_LCD_MISO   GPIO_NUM_19
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_DC     GPIO_NUM_4
#define PIN_LCD_BL     GPIO_NUM_0

/* ST7735 registers */
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

#define MADCTL_MY       0x80
#define MADCTL_MX       0x40
#define MADCTL_MV       0x20
#define MADCTL_RGB      0x00
#define MADCTL_BGR      0x08

static const char *TAG = "TEST2";
static esp_lcd_panel_io_handle_t s_io;

static void st7735_tx(int cmd, const void *param, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, param, len);
}

static void st7735_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* Exactly matches xiaomiao-loader's st7735_clear_black */
static void st7735_clear_black(void)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    memset(line, 0, sizeof(line));
    st7735_tx(ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {y >> 8, y & 0xFF, y2 >> 8, y2 & 0xFF};
        st7735_tx(ST7735_RASET, raset, sizeof(raset));
        st7735_tx(ST7735_RAMWR, line,
                  (uint16_t)(y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
}

/* Exactly matches xiaomiao-loader's st7735_init_black_tab_rot90 */
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
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t colmod[]  = {0x05};
    const uint8_t caset[]   = {0x00, 0x00, 0x00, LCD_NATIVE_H_RES - 1};
    const uint8_t raset[]   = {0x00, 0x00, 0x00, LCD_NATIVE_V_RES - 1};
    const uint8_t gp[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    const uint8_t gn[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};

    st7735_tx(ST7735_DISPOFF, NULL, 0);
    st7735_tx(ST7735_SWRESET, NULL, 0);
    st7735_delay(150);
    st7735_tx(ST7735_SLPOUT, NULL, 0);
    st7735_delay(500);
    st7735_tx(ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st7735_tx(ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st7735_tx(ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st7735_tx(ST7735_INVCTR,  invctr, sizeof(invctr));
    st7735_tx(ST7735_PWCTR1,  pwctr1, sizeof(pwctr1));
    st7735_tx(ST7735_PWCTR2,  pwctr2, sizeof(pwctr2));
    st7735_tx(ST7735_PWCTR3,  pwctr3, sizeof(pwctr3));
    st7735_tx(ST7735_PWCTR4,  pwctr4, sizeof(pwctr4));
    st7735_tx(ST7735_PWCTR5,  pwctr5, sizeof(pwctr5));
    st7735_tx(ST7735_VMCTR1,  vmctr1, sizeof(vmctr1));
    st7735_tx(ST7735_INVOFF,  NULL, 0);
    st7735_tx(ST7735_MADCTL,  madctl_d, sizeof(madctl_d));
    st7735_tx(ST7735_COLMOD,  colmod, sizeof(colmod));
    st7735_tx(ST7735_CASET,   caset, sizeof(caset));
    st7735_tx(ST7735_RASET,   raset, sizeof(raset));
    st7735_tx(ST7735_GMCTRP1, gp, sizeof(gp));
    st7735_tx(ST7735_GMCTRN1, gn, sizeof(gn));
    st7735_tx(ST7735_NORON,   NULL, 0);
    st7735_delay(10);
    st7735_tx(ST7735_MADCTL,  madctl_r, sizeof(madctl_r));
    /* CRITICAL: clear to black before DISPON */
    st7735_clear_black();
    /* DISPON will be sent below */
}

static void draw_rect(int x1, int y1, int x2, int y2, uint16_t color)
{
    uint8_t caset[] = {x1>>8, x1&0xFF, x2>>8, x2&0xFF};
    uint8_t raset[] = {y1>>8, y1&0xFF, y2>>8, y2&0xFF};
    st7735_tx(ST7735_CASET, caset, 4);
    st7735_tx(ST7735_RASET, raset, 4);

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int pixels = w * h;
    uint16_t *buf = heap_caps_aligned_alloc(64, pixels * 2, MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "malloc fail");
        return;
    }
    for (int i = 0; i < pixels; i++) buf[i] = color;
    esp_lcd_panel_io_tx_color(s_io, ST7735_RAMWR, (uint8_t*)buf, pixels * 2);
    heap_caps_free(buf);
}

void app_main(void)
{
    return_to_loader_setup();
    ESP_LOGI(TAG, "=== TEST v2 ===");

    /* Backlight ON */
    gpio_config_t bl = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL), .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BL, 0);

    /* SPI init */
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t cfg = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &s_io));

    /* Init ST7735 (exactly like xiaomiao-loader, includes clear_black) */
    st7735_init();
    ESP_LOGI(TAG, "Init done, clear_black done");

    /* Now send DISPON */
    st7735_tx(ST7735_DISPON, NULL, 0);
    st7735_delay(50);
    ESP_LOGI(TAG, "Display ON");

    /* Draw test pattern in rotated 160x128 mode */
    /* Draw full-screen red */
    draw_rect(0, 0, LCD_H_RES-1, LCD_V_RES-1, 0xF800);
    ESP_LOGI(TAG, "Red screen drawn");

    st7735_delay(2000);

    /* Draw green */
    draw_rect(0, 0, LCD_H_RES-1, LCD_V_RES-1, 0x07E0);
    ESP_LOGI(TAG, "Green screen drawn");

    /* Loop forever */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Alive...");
    }
}