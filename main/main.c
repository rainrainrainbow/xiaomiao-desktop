/*
 * xiaomiao-desktop - Desktop UI with App Grid
 *
 * ESP32-WROVER-B + ST7735 160x128 TFT + MicroSD + 6-key keypad.
 * NOTE: GPIO0 (backlight) is NOT touched - let hardware default control it.
 *
 * Layout (160x128 landscape):
 *   0-12px:  Status bar (brand + clock + battery)
 *   14-118px: App grid (2x2 or 1x2, icon left + label right)
 *   118-128px: Dock (3 dots)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
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

#define BTN_A          GPIO_NUM_34
#define BTN_B          GPIO_NUM_12
#define BTN_UP         GPIO_NUM_2
#define BTN_DOWN       GPIO_NUM_13
#define BTN_LEFT       GPIO_NUM_27
#define BTN_RIGHT      GPIO_NUM_35
#define BUTTON_ACTIVE_LEVEL  0
#define BUTTON_DEBOUNCE_MS   25

#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_6
#define BAT_VDIV_R1         9100
#define BAT_VDIV_R2         2400
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH    ADC_BITWIDTH_12

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

/* ========== UI layout constants (strict 160x128) ========== */
#define STATUS_H        12
#define DOCK_H          10
#define GRID_TOP        (STATUS_H + 2)   /* 14 */
#define GRID_BOTTOM     (LCD_V_RES - DOCK_H) /* 118 */
#define GRID_H          (GRID_BOTTOM - GRID_TOP) /* 104 */
#define GRID_PAD        4
#define GRID_GAP        4
#define ICON_SIZE       22
#define ICON_RADIUS     6
#define CELL_RADIUS     10

/* App icon colors (match HTML v9) */
#define COLOR_BLUE      0x3B82F6
#define COLOR_VIOLET    0x8B5CF6
#define COLOR_ROSE      0xF43F5E
#define COLOR_AMBER     0xF59E0B
#define COLOR_BG_DARK   0x0C0D10

/* ========== App definitions ========== */
typedef struct {
    const char *icon_text;  /* single-letter label inside icon */
    const char *label;      /* text label next to icon */
    uint32_t icon_color;    /* icon background color */
    uint32_t cell_tint;     /* cell background tint color */
} app_def_t;

#define NUM_APPS 4

static const app_def_t s_apps[NUM_APPS] = {
    { "P", "Phone",   COLOR_BLUE,   0x1A2A4A },
    { "G", "Games",   COLOR_VIOLET, 0x2A1A3A },
    { "C", "Camera",  COLOR_ROSE,   0x3A1A22 },
    { "M", "Music",   COLOR_AMBER,  0x3A2A10 },
};

/* ========== State ========== */
static lv_draw_buf_t s_draw_buf3;
static esp_lcd_panel_io_handle_t s_lcd_io;
static volatile bool s_first_flush;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_chars;
static lv_obj_t *s_bat_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_app_cells[NUM_APPS];
static lv_group_t *s_group;
static const char *TAG = "DESKTOP";

/* ========== Battery ========== */
static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = BAT_ADC_UNIT, .clk_src = ADC_RTC_CLK_SRC_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = BAT_ADC_ATTEN, .bitwidth = BAT_ADC_BITWIDTH };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));
    adc_cali_line_fitting_config_t cali_cfg = { .unit_id = BAT_ADC_UNIT, .atten = BAT_ADC_ATTEN, .bitwidth = BAT_ADC_BITWIDTH };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_chars));
}

static float battery_get_voltage(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw));
    int mv = 0;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_chars, raw, &mv));
    return (mv / 1000.0f) * (float)(BAT_VDIV_R1 + BAT_VDIV_R2) / (float)BAT_VDIV_R2;
}

static int battery_get_percent(float vbat)
{
    if (vbat >= 4.2f) return 100;
    if (vbat <= 3.0f) return 0;
    return (int)((vbat - 3.0f) / (4.2f - 3.0f) * 100.0f);
}

/* ========== Buttons ========== */
static const gpio_num_t s_btn_gpios[] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_B, BTN_A };
static const uint32_t s_btn_keys[] = { LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT, LV_KEY_ENTER, LV_KEY_ESC };
#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

static void buttons_init(void)
{
    uint64_t mask = 0, pullup = 0;
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        mask |= 1ULL << s_btn_gpios[i];
        if (s_btn_gpios[i] != GPIO_NUM_34 && s_btn_gpios[i] != GPIO_NUM_35)
            pullup |= 1ULL << s_btn_gpios[i];
    }
    gpio_config_t io = { .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
    if (pullup) {
        gpio_config_t pu = { .pin_bit_mask = pullup, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
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
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) { raw = (int)i; break; }
    }
    if (raw != last) { last = raw; changed_ms = now; if (raw < 0) stable = -1; }
    if (lv_tick_elaps(changed_ms) >= BUTTON_DEBOUNCE_MS) stable = last;
    if (stable >= 0) {
        last_key = s_btn_keys[stable];
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}

/* ========== ST7735 Driver ========== */
static void st7735_tx(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t len)
{
    esp_lcd_panel_io_tx_param(io, cmd, param, len);
}

static void st7735_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st7735_clear_black(esp_lcd_panel_io_handle_t io)
{
    static uint16_t line[LCD_H_RES * 8];
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    memset(line, 0, sizeof(line));
    st7735_tx(io, ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {(uint8_t)(y>>8), (uint8_t)(y&0xFF), (uint8_t)(y2>>8), (uint8_t)(y2&0xFF)};
        st7735_tx(io, ST7735_RASET, raset, sizeof(raset));
        st7735_tx(io, ST7735_RAMWR, line, (uint16_t)(y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
}

static void st7735_init(esp_lcd_panel_io_handle_t io)
{
    const uint8_t frmctr[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t pwctr1[] = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[] = {0xC5};
    const uint8_t pwctr3[] = {0x0A, 0x00};
    const uint8_t pwctr4[] = {0x8A, 0x2A};
    const uint8_t pwctr5[] = {0x8A, 0xEE};
    const uint8_t madctl[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
    const uint8_t madctl_r[] = {MADCTL_MX | MADCTL_MV | MADCTL_RGB};
    const uint8_t gp[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    const uint8_t gn[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
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
    st7735_tx(io, ST7735_MADCTL, madctl, sizeof(madctl));
    st7735_tx(io, ST7735_COLMOD, (uint8_t[]){0x05}, 1);
    st7735_tx(io, ST7735_CASET, (uint8_t[]){0,0,0,LCD_NATIVE_H_RES-1}, 4);
    st7735_tx(io, ST7735_RASET, (uint8_t[]){0,0,0,LCD_NATIVE_V_RES-1}, 4);
    st7735_tx(io, ST7735_GMCTRP1, gp, sizeof(gp));
    st7735_tx(io, ST7735_GMCTRN1, gn, sizeof(gn));
    st7735_tx(io, ST7735_NORON, NULL, 0);
    st7735_delay(10);
    st7735_tx(io, ST7735_MADCTL, madctl_r, sizeof(madctl_r));
    st7735_clear_black(io);
}

static esp_lcd_panel_io_handle_t lcd_init(void)
{
    spi_bus_config_t bus = { .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI, .miso_io_num = PIN_LCD_MISO, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2 };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t cfg = { .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS, .pclk_hz = LCD_PIXEL_CLOCK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &io));
    s_lcd_io = io;
    st7735_init(io);
    return io;
}

static bool flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    s_first_flush = true;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static bool s_display_on = false;

static void lcd_display_on(void)
{
    if (s_display_on || !s_lcd_io) return;
    st7735_tx(s_lcd_io, ST7735_DISPON, NULL, 0);
    st7735_delay(20);
    s_display_on = true;
    ESP_LOGI(TAG, "Display ON");
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(d);
    uint16_t x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    esp_lcd_panel_io_tx_param(io, ST7735_CASET, (uint8_t[]){x1>>8,x1&0xFF,x2>>8,x2&0xFF}, 4);
    esp_lcd_panel_io_tx_param(io, ST7735_RASET, (uint8_t[]){y1>>8,y1&0xFF,y2>>8,y2&0xFF}, 4);
    int sz = (x2-x1+1)*(y2-y1+1)*2;
    esp_lcd_panel_io_tx_color(io, ST7735_RAMWR, px, sz);
}

static void tick_cb(void *arg) { lv_tick_inc(1); }

static lv_display_t *display_init(esp_lcd_panel_io_handle_t io)
{
    lv_display_t *d = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_color_format_t cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint32_t stride = lv_draw_buf_width_to_stride(LCD_H_RES, cf);
    size_t sz = stride * LCD_V_RES;
    void *b1 = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_DMA);
    void *b2 = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_DMA);
    void *b3 = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_DMA);
    assert(b1 && b2 && b3);
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_draw_buf3, LCD_H_RES, LCD_V_RES, cf, stride, b3, sz);
    lv_display_set_3rd_draw_buffer(d, &s_draw_buf3);
    lv_display_set_user_data(d, io);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

/* ========== UI: App Grid (v9 design) ========== */

/*
 * Layout (4-app mode, 2x2 grid):
 *
 *   Grid area: x=4, y=14, w=152, h=104, pad=4, gap=4
 *   Each cell: w = (152 - 2*4 - 4) / 2 = 70, h = (104 - 2*4 - 4) / 2 = 46
 *   Inside cell: icon 22x22 at left, label text to right
 *
 * Layout (2-app mode, 1x2):
 *   Each cell: w = 152 - 2*4 = 144, h = (104 - 2*4 - 4) / 2 = 46
 */

static void app_cell_create(lv_obj_t *parent, const app_def_t *app, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, int idx)
{
    /* Cell container */
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, CELL_RADIUS, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(app->cell_tint), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_color(cell, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(cell, 6, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* Flex layout: icon left, label right */
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cell, 6, 0);

    /* Icon: colored rounded square with letter */
    lv_obj_t *icon = lv_obj_create(cell);
    lv_obj_set_size(icon, ICON_SIZE, ICON_SIZE);
    lv_obj_set_style_radius(icon, ICON_RADIUS, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(app->icon_color), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon_lbl = lv_label_create(icon);
    lv_label_set_text(icon_lbl, app->icon_text);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(icon_lbl);

    /* App label */
    lv_obj_t *label = lv_label_create(cell);
    lv_label_set_text(label, app->label);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8E8EC), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(label, 0, 0);
    lv_obj_set_style_pad_bottom(label, 0, 0);

    /* Add to group for keypad navigation */
    lv_group_add_obj(s_group, cell);
    s_app_cells[idx] = cell;
}

static void desktop_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* --- Status Bar (0-12px) --- */
    lv_obj_t *statusbar = lv_obj_create(scr);
    lv_obj_set_pos(statusbar, 0, 0);
    lv_obj_set_size(statusbar, LCD_H_RES, STATUS_H);
    lv_obj_set_style_bg_color(statusbar, lv_color_hex(0x0C0D10), 0);
    lv_obj_set_style_bg_opa(statusbar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(statusbar, 0, 0);
    lv_obj_set_style_pad_all(statusbar, 0, 0);
    lv_obj_set_style_pad_left(statusbar, 6, 0);
    lv_obj_set_style_pad_right(statusbar, 6, 0);
    lv_obj_clear_flag(statusbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(statusbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brand_lbl = lv_label_create(statusbar);
    lv_label_set_text(brand_lbl, "XIAOMIAO");
    lv_obj_set_style_text_color(brand_lbl, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(brand_lbl, &lv_font_montserrat_14, 0);

    /* Right side: clock + battery */
    lv_obj_t *right_cont = lv_obj_create(statusbar);
    lv_obj_remove_style_all(right_cont);
    lv_obj_set_flex_flow(right_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_cont, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_cont, 4, 0);
    lv_obj_clear_flag(right_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(right_cont);
    lv_label_set_text(s_time_label, "12:00");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_14, 0);

    s_bat_label = lv_label_create(right_cont);
    lv_label_set_text(s_bat_label, "85%");
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_14, 0);

    /* --- App Grid (14-118px) --- */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_set_pos(grid, GRID_PAD, GRID_TOP);
    lv_obj_set_size(grid, LCD_H_RES - 2 * GRID_PAD, GRID_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, GRID_PAD, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* 4-app mode: 2x2 grid */
    lv_coord_t grid_w = LCD_H_RES - 2 * GRID_PAD - 2 * GRID_PAD; /* inner width */
    lv_coord_t grid_h = GRID_H - 2 * GRID_PAD; /* inner height */
    lv_coord_t cell_w = (grid_w - GRID_GAP) / 2;  /* 70 */
    lv_coord_t cell_h = (grid_h - GRID_GAP) / 2;  /* 46 */

    /* Positions for 2x2 grid (relative to grid content area) */
    lv_coord_t x0 = 0, x1 = cell_w + GRID_GAP;
    lv_coord_t y0 = 0, y1 = cell_h + GRID_GAP;

    app_cell_create(grid, &s_apps[0], x0, y0, cell_w, cell_h, 0);
    app_cell_create(grid, &s_apps[1], x1, y0, cell_w, cell_h, 1);
    app_cell_create(grid, &s_apps[2], x0, y1, cell_w, cell_h, 2);
    app_cell_create(grid, &s_apps[3], x1, y1, cell_w, cell_h, 3);

    /* --- Dock (118-128px) --- */
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_set_pos(dock, 0, LCD_V_RES - DOCK_H);
    lv_obj_set_size(dock, LCD_H_RES, DOCK_H);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x08090C), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_90, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_border_side(dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dock, 10, 0);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(dock);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        if (i == 0) {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x444444), 0);
        }
    }
}

/* ========== Main ========== */
void app_main(void)
{
    return_to_loader_setup();
    battery_init();
    buttons_init();
    esp_lcd_panel_io_handle_t io = lcd_init();
    lv_init();
    lv_display_t *disp = display_init(io);
    s_group = lv_group_create();
    lv_group_set_default(s_group);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, disp);
    lv_indev_set_group(indev, s_group);
    lv_indev_set_read_cb(indev, keypad_read_cb);
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);
    desktop_ui_create();
    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();
    while (true) {
        lv_timer_handler();
        static uint32_t last_bat = 0;
        if (lv_tick_elaps(last_bat) > 5000) {
            last_bat = lv_tick_get();
            /* Battery */
            float v = battery_get_voltage();
            int pct = battery_get_percent(v);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", pct);
            if (s_bat_label) lv_label_set_text(s_bat_label, buf);
            /* Clock */
            time_t now;
            time(&now);
            struct tm *tm_info = localtime(&now);
            snprintf(buf, sizeof(buf), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
            if (s_time_label) lv_label_set_text(s_time_label, buf);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}