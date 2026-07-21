/*
 * xiaomiao-desktop - Metro UI for Xiaomiao Handheld
 *
 * ESP32-WROVER-B + ST7735 160x128 TFT + MicroSD + 6-key keypad.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "return_to_loader.h"

/* ── Hardware Constants ──────────────────────────────────────────────── */

#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160
#define LCD_H_RES           160
#define LCD_V_RES           128
#define LCD_DRAW_BUF_LINES  LCD_V_RES
#define LCD_DPI             60

#define PIN_LCD_SCLK   GPIO_NUM_18
#define PIN_LCD_MOSI   GPIO_NUM_23
#define PIN_LCD_MISO   GPIO_NUM_19
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_DC     GPIO_NUM_4

#define BUTTON_ACTIVE_LEVEL  0
#define BUTTON_DEBOUNCE_MS   25

#define LVGL_TICK_PERIOD_MS  1

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

/* Metro UI Colors */
#define METRO_BLACK    0x000000
#define METRO_WHITE    0xFFFFFF
#define METRO_ORANGE   0xFF6F00
#define METRO_INDIGO   0x3F51B5
#define METRO_TEAL     0x009688
#define METRO_BLUE_GREY 0x607D8B
#define METRO_BROWN    0x795548
#define METRO_AMBER    0xFF9800
#define METRO_DARK_BLUE 0x1A237E

/* ── Keypad ──────────────────────────────────────────────────────────── */

static const gpio_num_t s_btn_gpios[] = {
    GPIO_NUM_2,  GPIO_NUM_13, GPIO_NUM_27,
    GPIO_NUM_35, GPIO_NUM_34, GPIO_NUM_12,
};
static const uint32_t s_btn_keys[] = {
    LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT,
    LV_KEY_RIGHT, LV_KEY_ENTER, LV_KEY_ESC,
};
#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

static const char *TAG = "DESKTOP";

/* ── Globals ─────────────────────────────────────────────────────────── */

static lv_draw_buf_t s_draw_buf3;
static esp_lcd_panel_io_handle_t s_lcd_io;
static volatile bool s_first_flush;

/* ── Buttons ─────────────────────────────────────────────────────────── */

static void buttons_init(void)
{
    uint64_t mask = 0, pullup = 0;
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        mask |= 1ULL << s_btn_gpios[i];
        if (s_btn_gpios[i] != GPIO_NUM_34 && s_btn_gpios[i] != GPIO_NUM_35)
            pullup |= 1ULL << s_btn_gpios[i];
    }
    gpio_config_t io = {
        .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    if (pullup) {
        gpio_config_t pu = {
            .pin_bit_mask = pullup, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pu);
    }
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int last = -1, stable = -1;
    static uint32_t changed_ms = 0;
    static uint32_t last_key = LV_KEY_ENTER;
    int raw = -1;
    uint32_t now = lv_tick_get();

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            raw = (int)i;
            break;
        }
    }
    if (raw != last) {
        last = raw;
        changed_ms = now;
        if (raw < 0) stable = -1;
    }
    if (lv_tick_elaps(changed_ms) >= BUTTON_DEBOUNCE_MS)
        stable = last;

    if (stable >= 0) {
        last_key = s_btn_keys[stable];
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}

/* ── LCD ─────────────────────────────────────────────────────────────── */

static void st7735_tx(esp_lcd_panel_io_handle_t io, int cmd,
                      const void *param, size_t len)
{
    esp_lcd_panel_io_tx_param(io, cmd, param, len);
}

static void st7735_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st7735_init(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[]  = {0x01,0x2C,0x2D};
    const uint8_t frmctr3[] = {0x01,0x2C,0x2D,0x01,0x2C,0x2D};
    const uint8_t pwctr1[]  = {0xA2,0x02,0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A,0x00};
    const uint8_t pwctr4[]  = {0x8A,0x2A};
    const uint8_t pwctr5[]  = {0x8A,0xEE};
    const uint8_t madctl_d[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};
    const uint8_t colmod[] = {0x05};
    const uint8_t gp[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,
                          0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gn[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,
                          0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};

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
    st7735_tx(io, ST7735_CASET,
              (uint8_t[]){0,0,0,LCD_NATIVE_H_RES-1}, 4);
    st7735_tx(io, ST7735_RASET,
              (uint8_t[]){0,0,0,LCD_NATIVE_V_RES-1}, 4);
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
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t cfg = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &io));
    s_lcd_io = io;
    st7735_init(io);
    return io;
}

/* ── LVGL ────────────────────────────────────────────────────────────── */

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
        (uint8_t[]){x1>>8,x1&0xFF,x2>>8,x2&0xFF}, 4);
    esp_lcd_panel_io_tx_param(io, ST7735_RASET,
        (uint8_t[]){y1>>8,y1&0xFF,y2>>8,y2&0xFF}, 4);
    int sz = (x2-x1+1)*(y2-y1+1)*2;
    esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, sz);
}

static void tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

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

/* ── Metro UI ────────────────────────────────────────────────────────── */

static lv_obj_t *tile_create(lv_obj_t *parent, const char *title,
                             uint32_t color, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_radius(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    return tile;
}

static void metro_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(METRO_BLACK), 0);

    /* 标题 xiao miao desktop */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "xiao\nmiao");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 4);

    /* 状态栏 - 顶部 */
    lv_obj_t *topbar = lv_obj_create(scr);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, LCD_H_RES, 10);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_all(topbar, 0, 0);

    lv_obj_t *time_lbl = lv_label_create(topbar);
    lv_label_set_text(time_lbl, "12:42");
    lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(time_lbl, LV_ALIGN_RIGHT_MID, -2, 0);

    lv_obj_t *bat_lbl = lv_label_create(topbar);
    lv_label_set_text(bat_lbl, "BAT");
    lv_obj_set_style_text_color(bat_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(bat_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    /* Metro 磁贴区域 */
    lv_obj_t *desk = lv_obj_create(scr);
    lv_obj_set_pos(desk, 0, 22);
    lv_obj_set_size(desk, LCD_H_RES, LCD_V_RES - 22);
    lv_obj_set_style_bg_opa(desk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(desk, 0, 0);
    lv_obj_set_style_pad_all(desk, 0, 0);
    lv_obj_clear_flag(desk, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间磁贴 (2x2) */
    tile_create(desk, "12:42", METRO_ORANGE, 2, 2, 76, 38);

    /* 系统磁贴 (1x1) */
    tile_create(desk, "System", METRO_INDIGO, 80, 2, 36, 17);
    tile_create(desk, "Apps", METRO_TEAL, 118, 2, 36, 17);

    /* 中排 */
    tile_create(desk, "File", METRO_BLUE_GREY, 2, 42, 54, 17);
    tile_create(desk, "Settings", METRO_BROWN, 58, 42, 58, 17);

    /* 大应用区 */
    tile_create(desk, "Notes", METRO_AMBER, 2, 61, 74, 36);

    /* 底部 Dock */
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_set_pos(dock, 0, LCD_V_RES - 16);
    lv_obj_set_size(dock, LCD_H_RES, 14);
    lv_obj_set_style_bg_opa(dock, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(dock, lv_color_black(), 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_pad_all(dock, 1, 0);
    lv_obj_set_style_radius(dock, 0, 0);

    const char *dock_items[] = {"Phone", "Mail", "Photos", NULL};
    int gap = 4, item_w = 24;
    int total_w = 3 * item_w + 2 * gap;
    int start_x = (LCD_H_RES - total_w) / 2;

    for (int i = 0; dock_items[i] != NULL; i++) {
        lv_obj_t *btn = lv_btn_create(dock);
        lv_obj_set_size(btn, item_w, 12);
        lv_obj_set_pos(btn, start_x + i * (item_w + gap), 1);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_set_style_border_width(btn, 0, 0);

        lv_obj_t *dlbl = lv_label_create(btn);
        lv_label_set_text(dlbl, dock_items[i]);
        lv_obj_set_style_text_color(dlbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_14, 0);
        lv_obj_center(dlbl);
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    return_to_loader_setup();

    ESP_LOGI(TAG, "Metro Desktop boot");
    buttons_init();

    esp_lcd_panel_io_handle_t io = lcd_init();

    lv_init();
    lv_display_t *disp = display_init(io);

    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, disp);
    lv_indev_set_group(indev, group);
    lv_indev_set_read_cb(indev, keypad_read_cb);

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);

    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, LVGL_TICK_PERIOD_MS * 1000);

    metro_ui_create();
    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    st7735_tx(s_lcd_io, ST7735_DISPON, NULL, 0);
    st7735_delay(20);

    while (true) {
        uint32_t delay = lv_timer_handler();
        usleep(MAX(MIN(delay, 16), 1) * 1000);
    }
}
