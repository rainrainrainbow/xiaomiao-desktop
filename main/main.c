/*
 * XiaoMiao Desktop OS — ESP32-WROVER-B + ST7735 160x128 TFT + MicroSD + 6-key keypad
 * Based on xiaomiao-firmware framework
 *
 * Phase 2: Complete ESP-IDF Firmware Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"

#include "return_to_loader.h"
#include "xiaomiao_desktop.h"
#include "ui_widgets.h"
#include "app_launcher.h"
#include "task_manager.h"
#include "mpy/mpy_engine.h"

static const char *TAG = "xiaomiao-desktop";

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
#define MADCTL_MX       0x40
#define MADCTL_MY       0x80
#define MADCTL_MV       0x20
#define MADCTL_RGB      0x00

/* LCD globals */
static lv_draw_buf_t s_draw_buf3;
static volatile bool s_first_flush = false;

/* ===== ST7735 LCD ===== */
static void st7735_tx(esp_lcd_panel_io_handle_t io, int cmd,
                      const void *param, size_t len)
{
    esp_lcd_panel_io_tx_param(io, cmd, param, len);
}

static void st7735_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st7735_init(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[]  = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A, 0x00};
    const uint8_t pwctr4[]  = {0x8A, 0x2A};
    const uint8_t pwctr5[]  = {0x8A, 0xEE};
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};
    const uint8_t colmod[]  = {0x05};
    const uint8_t gp[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    const uint8_t gn[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};

    st7735_tx(io, ST7735_DISPOFF, NULL, 0);
    st7735_tx(io, ST7735_SWRESET, NULL, 0);
    st7735_delay(150);
    st7735_tx(io, ST7735_SLPOUT, NULL, 0);
    st7735_delay(500);
    st7735_tx(io, ST7735_FRMCTR1, frmctr, sizeof(frmctr));
    st7735_tx(io, ST7735_FRMCTR2, frmctr, sizeof(frmctr));
    st7735_tx(io, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3));
    st7735_tx(io, ST7735_INVCTR, (uint8_t[]){0x07}, 1);
    st7735_tx(io, ST7735_PWCTR1, pwctr1, sizeof(pwctr1));
    st7735_tx(io, ST7735_PWCTR2, pwctr2, sizeof(pwctr2));
    st7735_tx(io, ST7735_PWCTR3, pwctr3, sizeof(pwctr3));
    st7735_tx(io, ST7735_PWCTR4, pwctr4, sizeof(pwctr4));
    st7735_tx(io, ST7735_PWCTR5, pwctr5, sizeof(pwctr5));
    st7735_tx(io, ST7735_VMCTR1, (uint8_t[]){0x0E}, 1);
    st7735_tx(io, ST7735_INVOFF, NULL, 0);
    st7735_tx(io, ST7735_MADCTL, madctl_d, sizeof(madctl_d));
    st7735_tx(io, ST7735_COLMOD, colmod, sizeof(colmod));
    st7735_tx(io, ST7735_CASET, (uint8_t[]){0, 0, 0, LCD_NATIVE_H_RES - 1}, 4);
    st7735_tx(io, ST7735_RASET, (uint8_t[]){0, 0, 0, LCD_NATIVE_V_RES - 1}, 4);
    st7735_tx(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st7735_tx(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st7735_tx(io, ST7735_NORON, NULL, 0);
    st7735_delay(10);
    st7735_tx(io, ST7735_MADCTL, madctl_r, sizeof(madctl_r));
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &io));
    g_lcd_io = io;
    st7735_init(io);
    ESP_LOGI(TAG, "LCD initialized (160x128)");
    return io;
}

/* ===== SD Card ===== */
void sd_card_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 10000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_SD_CS;
    slot.wait_for_miso = 20;

    esp_vfs_fat_mount_config_t mcfg = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mcfg.format_if_mount_failed = false;
    mcfg.max_files = 4;

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mcfg, &card);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at /sdcard");
    } else {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(err));
    }
}

/* ===== LVGL Display ===== */
static bool flush_ready(esp_lcd_panel_io_handle_t io,
                        esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    s_first_flush = true;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(d);
    uint16_t x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    esp_lcd_panel_io_tx_param(io, ST7735_CASET,
        (uint8_t[]){x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF}, 4);
    esp_lcd_panel_io_tx_param(io, ST7735_RASET,
        (uint8_t[]){y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF}, 4);
    int sz = (x2 - x1 + 1) * (y2 - y1 + 1) * 2;
    esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, sz);
}

static void tick_cb(void *arg) { lv_tick_inc(1); }

static lv_display_t *display_init(esp_lcd_panel_io_handle_t io)
{
    lv_display_t *d = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, cf);
    size_t sz = stride * LCD_V_RES;

    void *b1 = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    void *b2 = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    void *b3 = spi_bus_dma_memory_alloc(LCD_HOST, sz, 0);
    assert(b1 && b2 && b3);

    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_V_RES, cf, stride, b3, sz);
    lv_display_set_3rd_draw_buffer(d, &s_draw_buf3);
    lv_display_set_user_data(d, io);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

/* ===== LVGL Task ===== */
static void lvgl_task(void *arg)
{
    /* Initialize all subsystems */
    task_manager_init();
    app_launcher_init();
    ui_main_init();

    /* Initialize MicroPython runtime */
    esp_err_t mpy_err = mpy_init();
    if (mpy_err != ESP_OK) {
        ESP_LOGW(TAG, "MicroPython init failed (%s), continuing without Python runtime",
                 esp_err_to_name(mpy_err));
    } else {
        /* Scan for installed .app packages — use heap allocation to avoid stack overflow */
        mpy_app_t *apps = heap_caps_malloc(sizeof(mpy_app_t) * MPY_MAX_APPS, MALLOC_CAP_SPIRAM);
        if (apps) {
            int app_count = mpy_scan_apps("/sdcard/apps", apps, MPY_MAX_APPS);
            ESP_LOGI(TAG, "Found %d MicroPython apps", app_count);
            free(apps);
        }
    }

    /* Start with boot screen, then auto-switch to desktop */
    nav_to(PAGE_BOOT);

    /* Wait for first flush, then turn on display */
    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    st7735_tx(g_lcd_io, ST7735_DISPON, NULL, 0);
    st7735_delay(20);

    /* Auto-navigate to desktop after boot animation */
    vTaskDelay(pdMS_TO_TICKS(2500));
    if (g_current_page == PAGE_BOOT) {
        nav_to(PAGE_DESKTOP);
    }

    /* Main LVGL loop */
    while (true) {
        uint32_t delay = lv_timer_handler();
        delay = MAX(MIN(delay, 16), 1);

        /* Process MicroPython pending events (scheduled callbacks, etc.) */
        mpy_process_events();

        usleep(delay * 1000);
    }
}

/* ===== Main Entry Point ===== */
void app_main(void)
{
    /* Return-to-Loader: must be first line */
    return_to_loader_setup();

    ESP_LOGI(TAG, "XiaoMiao Desktop OS booting...");

    /* Initialize time */
    setenv("TZ", "CST-8", 1);
    tzset();
    time_t now;
    time(&now);

    /* Initialize hardware */
    keypad_init();
    esp_lcd_panel_io_handle_t io = lcd_init();
    sd_card_init();

    /* Initialize LVGL */
    lv_init();
    g_display = display_init(io);

    /* Create keypad input device */
    g_group = lv_group_create();
    lv_group_set_default(g_group);
    g_indev = lv_indev_create();
    lv_indev_set_type(g_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(g_indev, g_display);
    lv_indev_set_group(g_indev, g_group);
    lv_indev_set_read_cb(g_indev, keypad_read_cb);
    lv_indev_set_long_press_time(g_indev, 360);
    lv_indev_set_long_press_repeat_time(g_indev, 130);

    /* Register flush callback */
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, g_display);

    /* Create LVGL tick timer */
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);

    /* Start LVGL task */
    xTaskCreate(lvgl_task, "lvgl", 10 * 1024, NULL, 5, NULL);
}