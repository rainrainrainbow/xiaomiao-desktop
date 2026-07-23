/*
 * xiaomiao-desktop v11 - Settings app + custom nav + fixed keypad
 *
 * Changes vs v10:
 *   - Layout: 4-app mode (2x2 vertical) vs 2-app mode (1x2 horizontal)
 *   - Keypad: BUILT-IN navigation. Use selected_idx and manual highlight,
 *     not LVGL group (which was broken due to keypad indev handler).
 *   - Settings app: real implementation with list navigation, brightness,
 *     sound toggle, theme switch, etc. Press B to return to desktop.
 *   - A/B buttons: A = enter / activate, B = back / cancel
 *
 * Hardware: ESP32-WROVER-B + ST7735 160x128 + 6-key keypad
 *   BTN_A=GPIO34 (shared with ADC1_CH6), BTN_B=12, UP=2, DOWN=13, LEFT=27, RIGHT=35
 *   All buttons active LOW with internal pullups (where possible)
 *
 * Architecture:
 *   - s_screen = "desktop" or "settings" (current screen)
 *   - s_selected_idx = current focused item in current screen
 *   - Press UP/DOWN/LEFT/RIGHT: move selection
 *   - Press A: launch app / activate item
 *   - Press B: return to previous screen
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
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

#define BTN_A          GPIO_NUM_34   /* ADC1_CH6 - SHARED with battery ADC! */
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
#define BAT_MIN_VALID_V     2.5f

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
#define ST7735_PWCTR2   0xC5
#define ST7735_PWCTR3   0xC6
#define ST7735_PWCTR4   0xC7
#define ST7735_PWCTR5   0xC8
#define ST7735_VMCTR1   0xC9
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

#define MADCTL_MX       0x40
#define MADCTL_MY       0x80
#define MADCTL_MV       0x20
#define MADCTL_RGB      0x00

#define STATUS_H        12
#define DOCK_H          10
#define HEADER_H        18   /* Settings header height */
#define GRID_TOP        (STATUS_H + 2)
#define GRID_BOTTOM     (LCD_V_RES - DOCK_H)
#define GRID_H          (GRID_BOTTOM - GRID_TOP)
#define GRID_PAD        4
#define GRID_GAP        4
#define ICON_SIZE_V     18   /* icon size in 2x2 mode (vertical layout) */
#define ICON_SIZE_H     18   /* icon size in 1x2 mode (horizontal layout) */
#define CELL_RADIUS     8

#define COLOR_BG_DARK   0x0C0D10
#define COLOR_SEL_BORDER 0xFFFFFF

/* App definitions */
typedef struct {
    const char *icon_text;
    const char *label;
    uint32_t icon_color;
    uint32_t cell_tint;
    void (*launch_cb)(void);  /* called when app is launched via A */
} app_def_t;

/* ========== State ========== */
static esp_lcd_panel_io_handle_t s_lcd_io;
static volatile bool s_first_flush;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_chars;
static lv_obj_t *s_bat_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_app_cells[8];
static lv_obj_t *s_settings_rows[8];
static lv_obj_t *s_settings_value_lbls[8];
static int s_app_count = 4;   /* 4 or 2 */
static int s_selected = 0;    /* current selection in current screen */
static int s_settings_count = 0;
static int s_settings_idx = 0;
static int s_last_pct = -1;
static int s_current_page = 0; /* current desktop page */
static int s_total_pages = 1;  /* total pages */

/* Settings values (mutable) */
static int s_setting_brightness = 75;
static int s_setting_sound_on = 1;
static int s_setting_theme = 0;  /* 0=Dark, 1=Light */
static int s_setting_wifi_on = 1;
static int s_setting_layout = 0; /* 0=4-apps, 1=2-apps */

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
static const gpio_num_t s_btn_gpios[] = {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B
};
#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

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
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    if (pullup) {
        gpio_config_t pu = {
            .pin_bit_mask = pullup, .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&pu);
    }
}

/* Buttons as raw button indices */
enum {
    BTN_IDX_UP = 0,
    BTN_IDX_DOWN = 1,
    BTN_IDX_LEFT = 2,
    BTN_IDX_RIGHT = 3,
    BTN_IDX_A = 4,
    BTN_IDX_B = 5,
};

static int scan_buttons(void)
{
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL)
            return (int)i;
    }
    return -1;
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
    const uint8_t madctl_0[] = {MADCTL_MX | MADCTL_MY | MADCTL_RGB};
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
    st7735_tx(io, ST7735_MADCTL, madctl_0, sizeof(madctl_0));
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
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK, .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t cfg = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10
    };
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
    assert(b1 && b2);
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(d, io);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

/* ========== Screen state ========== */
typedef enum {
    SCREEN_DESKTOP = 0,
    SCREEN_SETTINGS = 1,
    SCREEN_APP_PLACEHOLDER = 2,
} screen_t;

static screen_t s_screen = SCREEN_DESKTOP;

/* Theme colors (defined early so all UI builders can use them) */
static uint32_t theme_bg(void) { return s_setting_theme ? 0xF0F0F0 : COLOR_BG_DARK; }
static uint32_t theme_text(void) { return s_setting_theme ? 0x1A1A1A : 0xE8E8EC; }
static uint32_t theme_text_dim(void) { return s_setting_theme ? 0x666666 : 0x9AA0AC; }
static uint32_t theme_header_bg(void) { return s_setting_theme ? 0xD8D8D8 : 0x16181E; }
static uint32_t theme_border(void) { return s_setting_theme ? 0xCCCCCC : 0x222222; }
static uint32_t theme_sel_bg(void) { return s_setting_theme ? 0xD0E0FF : 0x2A3A5A; }

/* Forward decls */
static void desktop_rebuild(void);
static void settings_rebuild(void);
static void settings_activate(int idx);
static void settings_back(void);
static void save_settings_to_nvs(void);
static void load_settings_from_nvs(void);

/* ========== App launch handlers (forward) ========== */
static void launch_settings(void);
static void launch_phone(void);
static void launch_games(void);
static void launch_camera(void);
static void launch_music(void);
static void launch_browser(void);
static void launch_notes(void);
static void launch_about(void);

/* Apps (8 max) */
static const app_def_t s_apps[8] = {
    { "S", "Settings", 0x3B82F6, 0x1A2A4A, launch_settings },
    { "P", "Phone",    0x8B5CF6, 0x2A1A3A, launch_phone },
    { "G", "Games",    0xF43F5E, 0x3A1A22, launch_games },
    { "C", "Camera",   0xF59E0B, 0x3A2A10, launch_camera },
    { "M", "Music",    0x22C55E, 0x1A3A22, launch_music },
    { "B", "Browser",  0x06B6D4, 0x1A2A3A, launch_browser },
    { "T", "Notes",    0xEC4899, 0x3A1A2A, launch_notes },
    { "?", "About",    0x64748B, 0x1A1A1A, launch_about },
};

/* ========== UI builders ========== */

static lv_obj_t *make_statusbar(lv_obj_t *scr)
{
    lv_obj_t *sb = lv_obj_create(scr);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_size(sb, LCD_H_RES, STATUS_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(0x0C0D10), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_90, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_set_style_pad_left(sb, 4, 0);
    lv_obj_set_style_pad_right(sb, 4, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *brand = lv_label_create(sb);
    lv_label_set_text(brand, "XIAOMIAO");
    lv_obj_set_style_text_color(brand, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_12, 0);

    lv_obj_t *rc = lv_obj_create(sb);
    lv_obj_remove_style_all(rc);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rc, 4, 0);
    lv_obj_clear_flag(rc, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(rc);
    lv_label_set_text(s_time_label, "12:00");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xD8D8D8), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_12, 0);

    s_bat_label = lv_label_create(rc);
    lv_label_set_text(s_bat_label, "85%");
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_12, 0);

    return sb;
}

static lv_obj_t *make_dock(lv_obj_t *scr, int active_idx)
{
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
    lv_obj_set_style_pad_column(dock, 8, 0);

    /* Dynamic dot count based on total pages */
    int dot_count = (s_total_pages > 0) ? s_total_pages : 1;
    for (int i = 0; i < dot_count; i++) {
        lv_obj_t *dot = lv_obj_create(dock);
        lv_obj_set_size(dot, 3, 3);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        if (i == active_idx) {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x444444), 0);
        }
    }
    return dock;
}

static void highlight_cell(int idx)
{
    for (int i = 0; i < s_app_count; i++) {
        if (!s_app_cells[i]) continue;
        if (i == idx) {
            lv_obj_set_style_border_width(s_app_cells[i], 2, 0);
            lv_obj_set_style_border_color(s_app_cells[i], lv_color_hex(COLOR_SEL_BORDER), 0);
        } else {
            lv_obj_set_style_border_width(s_app_cells[i], 1, 0);
            lv_obj_set_style_border_color(s_app_cells[i], lv_color_hex(0x1A1A1A), 0);
        }
    }
}

static void highlight_setting(int idx)
{
    for (int i = 0; i < s_settings_count; i++) {
        if (!s_settings_rows[i]) continue;
        if (i == idx) {
            lv_obj_set_style_bg_color(s_settings_rows[i], lv_color_hex(theme_sel_bg()), 0);
            lv_obj_set_style_bg_opa(s_settings_rows[i], LV_OPA_COVER, 0);
            /* Scroll to make selected item visible */
            lv_obj_scroll_to_view(s_settings_rows[i], LV_ANIM_ON);
        } else {
            lv_obj_set_style_bg_opa(s_settings_rows[i], LV_OPA_TRANSP, 0);
        }
    }
}

/* Create a single app cell.
 * vertical = true for 2x2 mode (icon top, label bottom)
 * vertical = false for 1x2 mode (icon left, label right) */
static lv_obj_t *create_app_cell(lv_obj_t *parent, const app_def_t *app,
                                  lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                  bool vertical)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_style_radius(cell, CELL_RADIUS, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(app->cell_tint), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x1A1A1A), 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    if (vertical) {
        /* 2x2: ICON TOP, LABEL BOTTOM */
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 2, 0);
        lv_obj_set_style_pad_top(cell, 4, 0);
        lv_obj_set_style_pad_bottom(cell, 4, 0);

        lv_obj_t *icon = lv_obj_create(cell);
        lv_obj_set_size(icon, ICON_SIZE_V, ICON_SIZE_V);
        lv_obj_set_style_radius(icon, 4, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(app->icon_color), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *il = lv_label_create(icon);
        lv_label_set_text(il, app->icon_text);
        lv_obj_set_style_text_color(il, lv_color_white(), 0);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_12, 0);
        lv_obj_center(il);

        lv_obj_t *label = lv_label_create(cell);
        lv_label_set_text(label, app->label);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE8E8EC), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    } else {
        /* 1x2: ICON LEFT, LABEL RIGHT */
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cell, 8, 0);
        lv_obj_set_style_pad_left(cell, 8, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);

        lv_obj_t *icon = lv_obj_create(cell);
        lv_obj_set_size(icon, ICON_SIZE_H, ICON_SIZE_H);
        lv_obj_set_style_radius(icon, 4, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(app->icon_color), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *il = lv_label_create(icon);
        lv_label_set_text(il, app->icon_text);
        lv_obj_set_style_text_color(il, lv_color_white(), 0);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_12, 0);
        lv_obj_center(il);

        lv_obj_t *label = lv_label_create(cell);
        lv_label_set_text(label, app->label);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE8E8EC), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    }
    return cell;
}

/* Destroy all desktop children except persistent (statusbar etc) and rebuild grid */
static void desktop_rebuild(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* Clean: delete everything except statusbar */
    uint32_t cnt = lv_obj_get_child_count(scr);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        lv_obj_delete(c);
    }
    /* clear cell/setting arrays */
    for (int i = 0; i < 8; i++) {
        s_app_cells[i] = NULL;
        s_settings_rows[i] = NULL;
        s_settings_value_lbls[i] = NULL;
    }
    /* recreate */
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    make_statusbar(scr);

    /* App grid */
    lv_coord_t grid_w = LCD_H_RES - 2 * GRID_PAD;
    lv_coord_t grid_h = GRID_H;
    lv_coord_t inner_w = grid_w - 2 * GRID_PAD;
    lv_coord_t inner_h = grid_h - 2 * GRID_PAD;

    bool vertical;
    lv_coord_t cell_w, cell_h;
    if (s_app_count == 4) {
        vertical = true;
        cell_w = (inner_w - GRID_GAP) / 2;
        cell_h = (inner_h - GRID_GAP) / 2;
    } else {
        vertical = false;
        cell_w = inner_w;
        cell_h = (inner_h - GRID_GAP) / 2;
    }

    /* Calculate total apps: built-in + SD card apps */
    int total_builtin = 8;  /* s_apps array size */
    int total_apps = total_builtin + s_sd_app_count;
    
    s_total_pages = (total_apps + s_app_count - 1) / s_app_count;
    if (s_current_page >= s_total_pages) {
        s_current_page = s_total_pages - 1;
    }

    /* Calculate which apps to show on current page */
    int start_app = s_current_page * s_app_count;
    int apps_on_page = s_app_count;
    if (start_app + apps_on_page > total_apps) {
        apps_on_page = total_apps - start_app;
    }

    lv_coord_t x[8], y[8];
    if (s_app_count == 4) {
        x[0] = 0;          y[0] = 0;
        x[1] = cell_w + GRID_GAP; y[1] = 0;
        x[2] = 0;          y[2] = cell_h + GRID_GAP;
        x[3] = cell_w + GRID_GAP; y[3] = cell_h + GRID_GAP;
    } else {
        x[0] = 0; y[0] = 0;
        x[1] = 0; y[1] = cell_h + GRID_GAP;
    }

    for (int i = 0; i < apps_on_page; i++) {
        int app_idx = start_app + i;
        
        /* Check if this is a built-in app or SD card app */
        if (app_idx < total_builtin) {
            /* Built-in app */
            s_app_cells[i] = create_app_cell(scr, &s_apps[app_idx], x[i], y[i], cell_w, cell_h, vertical);
        } else {
            /* SD card app */
            int sd_idx = app_idx - total_builtin;
            if (sd_idx < s_sd_app_count) {
                /* Create a temporary app_def_t for SD card app */
                app_def_t sd_app_def;
                sd_app_def.icon_text = s_sd_apps[sd_idx].icon;
                sd_app_def.label = s_sd_apps[sd_idx].name;
                sd_app_def.icon_color = s_sd_apps[sd_idx].color;
                sd_app_def.cell_tint = (s_sd_apps[sd_idx].color >> 1) & 0x7F7F7F; /* Darker tint */
                sd_app_def.launch_cb = NULL; /* Will be handled in desktop_activate */
                
                s_app_cells[i] = create_app_cell(scr, &sd_app_def, x[i], y[i], cell_w, cell_h, vertical);
            }
        }
        
        /* Adjust cell into grid position */
        if (s_app_cells[i]) {
            lv_obj_set_pos(s_app_cells[i], GRID_PAD + x[i], GRID_TOP + y[i]);
        }
    }

    make_dock(scr, s_current_page);

    s_selected = 0;
    s_screen = SCREEN_DESKTOP;
    highlight_cell(s_selected);
    ESP_LOGI(TAG, "Desktop rebuilt: page %d/%d, %d apps (%d built-in + %d SD) (%s)", 
             s_current_page + 1, s_total_pages, apps_on_page, 
             (start_app < total_builtin) ? (apps_on_page < (total_builtin - start_app) ? apps_on_page : (total_builtin - start_app)) : 0,
             (start_app >= total_builtin) ? apps_on_page : ((start_app + apps_on_page > total_builtin) ? (start_app + apps_on_page - total_builtin) : 0),
             vertical ? "2x2 vertical" : "1x2 horizontal");
}

/* Settings row struct */
typedef struct {
    const char *label;
    const char *type;  /* "toggle", "select", "value", "action" */
    int *value;
    int min, max, step;
    void (*on_activate)(int *v);
    uint32_t icon_color;
    const char *icon_letter;
} setting_item_t;

#define NUM_SETTINGS 7
static const setting_item_t s_settings[NUM_SETTINGS] = {
    { "WiFi",       "toggle", &s_setting_wifi_on, 0, 1, 1, NULL, 0x3B82F6, "W" },
    { "Brightness", "value",  &s_setting_brightness, 10, 100, 10, NULL, 0xF59E0B, "B" },
    { "Sound",      "toggle", &s_setting_sound_on, 0, 1, 1, NULL, 0x22C55E, "S" },
    { "Theme",      "select", &s_setting_theme, 0, 1, 1, NULL, 0xEC4899, "T" },
    { "Layout",     "select", &s_setting_layout, 0, 1, 1, NULL, 0x8B5CF6, "L" },
    { "About",      "action", NULL, 0, 0, 0, NULL, 0x64748B, "A" },
    { "Reset",      "action", NULL, 0, 0, 0, NULL, 0xEF4444, "R" },
};

/* Format value display for a setting */
static void setting_value_str(int idx, char *buf, size_t bufsz)
{
    const setting_item_t *s = &s_settings[idx];
    if (strcmp(s->type, "toggle") == 0) {
        snprintf(buf, bufsz, "%s", *s->value ? "On" : "Off");
    } else if (strcmp(s->type, "value") == 0) {
        snprintf(buf, bufsz, "%d%%", *s->value);
    } else if (strcmp(s->type, "select") == 0) {
        if (idx == 3) snprintf(buf, bufsz, "%s", *s->value ? "Light" : "Dark");
        else if (idx == 4) snprintf(buf, bufsz, "%s", *s->value ? "1x2" : "2x2");
        else snprintf(buf, bufsz, "%d", *s->value);
    } else {
        snprintf(buf, bufsz, ">");
    }
}

static void settings_rebuild(void)
{
    lv_obj_t *scr = lv_screen_active();
    /* delete all */
    uint32_t cnt = lv_obj_get_child_count(scr);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        lv_obj_delete(c);
    }
    for (int i = 0; i < 8; i++) {
        s_app_cells[i] = NULL;
        s_settings_rows[i] = NULL;
        s_settings_value_lbls[i] = NULL;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(theme_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(theme_header_bg()), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(theme_border()), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_left(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 4, 0);

    /* Back button (B) */
    lv_obj_t *back = lv_obj_create(hdr);
    lv_obj_set_size(back, 10, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2D36), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "<");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_center(bl);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

    /* Battery in header (compact) */
    s_bat_label = lv_label_create(hdr);
    char bbuf[16];
    snprintf(bbuf, sizeof(bbuf), "%d%%", s_last_pct >= 0 ? s_last_pct : 85);
    lv_label_set_text(s_bat_label, bbuf);
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_font(s_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(s_bat_label, 60, 0);

    /* Settings list (scrollable) */
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_pos(list, 0, HEADER_H);
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - HEADER_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    /* Enable scrolling */
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_NONE);

    lv_coord_t row_h = 18;
    for (int i = 0; i < NUM_SETTINGS; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x1F2228), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_set_style_pad_right(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 4, 0);

        /* icon */
        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_set_size(icon, 12, 12);
        lv_obj_set_style_radius(icon, 2, 0);
        lv_obj_set_style_bg_color(icon, lv_color_hex(s_settings[i].icon_color), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *il = lv_label_create(icon);
        lv_label_set_text(il, s_settings[i].icon_letter);
        lv_obj_set_style_text_color(il, lv_color_white(), 0);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_14, 0);
        lv_obj_center(il);

        /* label */
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s_settings[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(theme_text()), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_left(lbl, 0, 0);

        /* value */
        lv_obj_t *vbox = lv_obj_create(row);
        lv_obj_remove_style_all(vbox);
        lv_obj_set_flex_flow(vbox, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(vbox, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(vbox, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_column(vbox, 4, 0);

        lv_obj_t *vlbl = lv_label_create(vbox);
        char vbuf[16];
        setting_value_str(i, vbuf, sizeof(vbuf));
        lv_label_set_text(vlbl, vbuf);
        lv_obj_set_style_text_color(vlbl, lv_color_hex(theme_text_dim()), 0);
        lv_obj_set_style_text_font(vlbl, &lv_font_montserrat_14, 0);

        s_settings_rows[i] = row;
        s_settings_value_lbls[i] = vlbl;
    }
    s_settings_count = NUM_SETTINGS;
    s_settings_idx = 0;
    s_screen = SCREEN_SETTINGS;
    highlight_setting(s_settings_idx);
}

/* ========== App placeholder screens ========== */
static void app_placeholder_show(const char *title, const char *subtitle, uint32_t color)
{
    lv_obj_t *scr = lv_screen_active();
    uint32_t cnt = lv_obj_get_child_count(scr);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(scr, i));
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x16181E), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_left(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back = lv_obj_create(hdr);
    lv_obj_set_size(back, 10, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2D36), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "<");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_center(bl);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_12, 0);

    /* Content */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, HEADER_H);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - HEADER_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 8, 0);

    /* Icon */
    lv_obj_t *icon = lv_obj_create(content);
    lv_obj_set_size(icon, 40, 40);
    lv_obj_set_style_radius(icon, 8, 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sub = lv_label_create(content);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9AA0AC), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "Press B to return");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);

    s_screen = SCREEN_APP_PLACEHOLDER; /* Use dedicated state for app screens */
}

/* ========== SD card app scanning ========== */
/* TODO v14: Integrate SD card hardware (HSPI SDSPI driver).
 *
 * Required hardware configuration:
 *   - SDSPI bus: SCLK=MOSI=MISO=<TBD>, CS=<TBD>
 *   - Mount point: "/sdcard" (FAT32)
 *
 * Required sdkconfig.defaults additions:
 *   CONFIG_FATFS_LONG_FILENAME=y
 *   CONFIG_FATFS_MAX_LFN=255
 *   CONFIG_SDPHYSIC_DRIVER=y
 *   CONFIG_COMPONENT_SDSPI_ENABLED=y
 *
 * Application directory layout (after SD card enabled):
 *   /sdcard/xiaomiao-apps/
 *     calculator/
 *       app.json         (name, icon, color, entry, version)
 *       main.py          (MicroPython entry)
 *     tetris/
 *       app.json
 *       main.py
 *       icon.bmp         (optional)
 *
 * Application launching will use MicroPython firmware component.
 * See: https://github.com/micropython/micropython/tree/master/ports/esp32
 */

#define MAX_SD_APPS 16

typedef struct {
    char name[32];
    char icon[8];
    uint32_t color;
    char entry[32];
} sd_app_info_t;

static sd_app_info_t s_sd_apps[MAX_SD_APPS];
static int s_sd_app_count = 0;

/* v14: enable SD card scanning here after hardware integration */
static void scan_sdcard_apps(void)
{
    s_sd_app_count = 0;
    ESP_LOGW(TAG, "SD card scanning disabled (driver not yet integrated)");
}

/* ========== Reset function ========== */
/* NOTE: Persistent settings (NVS) deferred to v14.
 * The infrastructure (save/load/reset) is in place but the NVS component
 * needs proper CMakeLists.txt configuration. For now we just log. */
static void save_settings_to_nvs(void)
{
    /* v14: re-enable NVS persistence */
}
static void load_settings_from_nvs(void)
{
    /* v14: re-enable NVS persistence */
}
static void reset_settings(void)
{
    s_setting_brightness = 75;
    s_setting_sound_on = 1;
    s_setting_theme = 0;
    s_setting_wifi_on = 1;
    s_setting_layout = 0;
    s_app_count = 4;
    s_current_page = 0;
    ESP_LOGI(TAG, "Settings reset to defaults (in-memory only)");
}

/* Theme colors already defined above */

/* ========== App launch handlers ========== */
static void launch_settings(void) {
    ESP_LOGI(TAG, "Launch: Settings");
    settings_rebuild();
}
static void launch_phone(void) {
    ESP_LOGI(TAG, "Launch: Phone");
    app_placeholder_show("Phone", "Dialer coming soon...", 0x8B5CF6);
}
static void launch_games(void) {
    ESP_LOGI(TAG, "Launch: Games");
    app_placeholder_show("Games", "Game library loading...", 0xF43F5E);
}
static void launch_camera(void) {
    ESP_LOGI(TAG, "Launch: Camera");
    app_placeholder_show("Camera", "Camera preview...", 0xF59E0B);
}
static void launch_music(void) {
    ESP_LOGI(TAG, "Launch: Music");
    app_placeholder_show("Music", "Now playing...", 0x22C55E);
}
static void launch_browser(void) {
    ESP_LOGI(TAG, "Launch: Browser");
    app_placeholder_show("Browser", "Web browser...", 0x06B6D4);
}
static void launch_notes(void) {
    ESP_LOGI(TAG, "Launch: Notes");
    app_placeholder_show("Notes", "Note editor...", 0xEC4899);
}
static void launch_about(void) {
    ESP_LOGI(TAG, "Launch: About");
    /* Show about screen with device info */
    lv_obj_t *scr = lv_screen_active();
    uint32_t cnt = lv_obj_get_child_count(scr);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        lv_obj_delete(lv_obj_get_child(scr, i));
    }
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_DARK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Header */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x16181E), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_pad_left(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back = lv_obj_create(hdr);
    lv_obj_set_size(back, 10, 10);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2A2D36), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "<");
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_center(bl);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, "About");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_12, 0);

    /* Content */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_pos(content, 0, HEADER_H);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - HEADER_H);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 4, 0);

    lv_obj_t *l1 = lv_label_create(content);
    lv_label_set_text(l1, "XiaoMiao Desktop");
    lv_obj_set_style_text_color(l1, lv_color_white(), 0);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);

    lv_obj_t *l2 = lv_label_create(content);
    lv_label_set_text(l2, "v12.0");
    lv_obj_set_style_text_color(l2, lv_color_hex(0x9AA0AC), 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);

    lv_obj_t *l3 = lv_label_create(content);
    lv_label_set_text(l3, "ESP32-WROVER-B");
    lv_obj_set_style_text_color(l3, lv_color_hex(0x9AA0AC), 0);
    lv_obj_set_style_text_font(l3, &lv_font_montserrat_12, 0);

    lv_obj_t *l4 = lv_label_create(content);
    lv_label_set_text(l4, "ST7735 160x128");
    lv_obj_set_style_text_color(l4, lv_color_hex(0x9AA0AC), 0);
    lv_obj_set_style_text_font(l4, &lv_font_montserrat_12, 0);

    lv_obj_t *l5 = lv_label_create(content);
    lv_label_set_text(l5, "LVGL 9.5.0");
    lv_obj_set_style_text_color(l5, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(l5, &lv_font_montserrat_12, 0);

    s_screen = SCREEN_SETTINGS;
}

/* ========== Input handling ========== */
static void settings_activate(int idx)
{
    const setting_item_t *s = &s_settings[idx];
    if (strcmp(s->type, "toggle") == 0) {
        *s->value = !(*s->value);
    } else if (strcmp(s->type, "value") == 0) {
        *s->value += s->step;
        if (*s->value > s->max) *s->value = s->min;
    } else if (strcmp(s->type, "select") == 0) {
        *s->value = !(*s->value);
        if (idx == 4) {
            /* Layout changed */
            s_setting_layout = *s->value;
            s_app_count = s_setting_layout ? 2 : 4;
        }
    } else if (strcmp(s->type, "action") == 0) {
        if (idx == 5) {
            /* About - show about screen */
            launch_about();
            return;
        } else if (idx == 6) {
            /* Reset - restore defaults */
            reset_settings();
            settings_rebuild();
            return;
        }
    }
    /* Update value label */
    char vbuf[16];
    setting_value_str(idx, vbuf, sizeof(vbuf));
    if (s_settings_value_lbls[idx]) lv_label_set_text(s_settings_value_lbls[idx], vbuf);

    /* Auto-save settings to NVS after every change */
    save_settings_to_nvs();
}

static void settings_back(void)
{
    ESP_LOGI(TAG, "Settings: back");
    desktop_rebuild();
}

static void desktop_activate(int idx)
{
    /* Calculate global app index based on current page */
    int global_idx = s_current_page * s_app_count + idx;
    ESP_LOGI(TAG, "Desktop: launch #%d (page %d, local #%d, global #%d)", idx, s_current_page, idx, global_idx);
    
    int total_builtin = 8;
    
    if (global_idx < total_builtin) {
        /* Built-in app */
        if (s_apps[global_idx].launch_cb) {
            s_apps[global_idx].launch_cb();
        }
    } else {
        /* SD card app */
        int sd_idx = global_idx - total_builtin;
        if (sd_idx < s_sd_app_count) {
            sd_app_info_t *app = &s_sd_apps[sd_idx];
            ESP_LOGI(TAG, "Launch SD app: %s (entry=%s)", app->name, app->entry);
            
            /* Show placeholder for SD card app */
            char subtitle[64];
            snprintf(subtitle, sizeof(subtitle), "SD card app: %s", app->entry);
            app_placeholder_show(app->name, subtitle, app->color);
        }
    }
}

static void desktop_back(void)
{
    /* No-op on desktop */
    ESP_LOGI(TAG, "Desktop: B (no-op)");
}

/* Main input dispatcher */
static void handle_input(int raw_idx)
{
    if (raw_idx < 0) return;

    /* Handle app placeholder screen - B key returns to desktop */
    if (s_screen == SCREEN_APP_PLACEHOLDER) {
        if (raw_idx == BTN_IDX_B) {
            ESP_LOGI(TAG, "App placeholder: B -> desktop");
            desktop_rebuild();
        }
        return;
    }

    if (s_screen == SCREEN_DESKTOP) {
        if (raw_idx == BTN_IDX_LEFT) {
            if (s_app_count == 4) {
                /* 2x2: LEFT moves left in row */
                if (s_selected % 2 == 1) {
                    s_selected--;
                } else if (s_current_page > 0) {
                    /* At left edge, switch to previous page */
                    s_current_page--;
                    s_selected = s_app_count - 1;
                    desktop_rebuild();
                    save_settings_to_nvs();
                    return;
                }
            } else {
                if (s_selected > 0) s_selected--;
            }
            highlight_cell(s_selected);
        } else if (raw_idx == BTN_IDX_RIGHT) {
            if (s_app_count == 4) {
                /* 2x2: RIGHT moves right in row */
                if (s_selected % 2 == 0) {
                    s_selected++;
                } else if (s_current_page < s_total_pages - 1) {
                    /* At right edge, switch to next page */
                    s_current_page++;
                    s_selected = 0;
                    desktop_rebuild();
                    save_settings_to_nvs();
                    return;
                }
            } else {
                if (s_selected < s_app_count - 1) s_selected++;
            }
            highlight_cell(s_selected);
        } else if (raw_idx == BTN_IDX_UP) {
            if (s_app_count == 4) {
                /* 2x2: UP moves up in column */
                if (s_selected >= 2) s_selected -= 2;
            } else {
                if (s_selected > 0) s_selected--;
            }
            highlight_cell(s_selected);
        } else if (raw_idx == BTN_IDX_DOWN) {
            if (s_app_count == 4) {
                /* 2x2: DOWN moves down in column */
                if (s_selected < 2) s_selected += 2;
            } else {
                if (s_selected < s_app_count - 1) s_selected++;
            }
            highlight_cell(s_selected);
        } else if (raw_idx == BTN_IDX_A) {
            desktop_activate(s_selected);
        } else if (raw_idx == BTN_IDX_B) {
            desktop_back();
        }
    } else if (s_screen == SCREEN_SETTINGS) {
        if (raw_idx == BTN_IDX_UP) {
            if (s_settings_idx > 0) s_settings_idx--;
            highlight_setting(s_settings_idx);
        } else if (raw_idx == BTN_IDX_DOWN) {
            if (s_settings_idx < s_settings_count - 1) s_settings_idx++;
            highlight_setting(s_settings_idx);
        } else if (raw_idx == BTN_IDX_LEFT) {
            const setting_item_t *s = &s_settings[s_settings_idx];
            if (strcmp(s->type, "value") == 0) {
                *s->value -= s->step;
                if (*s->value < s->min) *s->value = s->max;
                char vbuf[16];
                setting_value_str(s_settings_idx, vbuf, sizeof(vbuf));
                if (s_settings_value_lbls[s_settings_idx])
                    lv_label_set_text(s_settings_value_lbls[s_settings_idx], vbuf);
                save_settings_to_nvs();
            }
        } else if (raw_idx == BTN_IDX_RIGHT) {
            const setting_item_t *s = &s_settings[s_settings_idx];
            if (strcmp(s->type, "value") == 0) {
                *s->value += s->step;
                if (*s->value > s->max) *s->value = s->min;
                char vbuf[16];
                setting_value_str(s_settings_idx, vbuf, sizeof(vbuf));
                if (s_settings_value_lbls[s_settings_idx])
                    lv_label_set_text(s_settings_value_lbls[s_settings_idx], vbuf);
                save_settings_to_nvs();
            }
        } else if (raw_idx == BTN_IDX_A) {
            settings_activate(s_settings_idx);
        } else if (raw_idx == BTN_IDX_B) {
            settings_back();
        }
    }
}

/* Keypad indev read callback (used for raw input fallback) */
static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* We don't actually use LVGL's keypad; navigation is custom in main loop.
     * But we still need to provide valid data. */
    static int last = -1;
    int raw = scan_buttons();
    if (raw != last) {
        last = raw;
    }
    if (raw >= 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = 0;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
    }
}

/* ========== Main ========== */
void app_main(void)
{
    return_to_loader_setup();

    /* Load saved settings from NVS (currently stubbed - v14 TODO) */
    load_settings_from_nvs();

    battery_init();
    buttons_init();
    esp_lcd_panel_io_handle_t io = lcd_init();
    lv_init();
    lv_display_t *disp = display_init(io);

    /* We don't actually need LVGL keypad for navigation, but create one to satisfy LVGL */
    lv_group_t *grp = lv_group_create();
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, disp);
    lv_indev_set_group(indev, grp);
    lv_indev_set_read_cb(indev, keypad_read_cb);

    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);

    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);

    /* Initialize battery */
    {
        float v = battery_get_voltage();
        s_last_pct = battery_get_percent(v);
    }

    /* Scan SD card for apps */
    scan_sdcard_apps();

    desktop_rebuild();

    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();

    ESP_LOGI(TAG, "=== Xiaomiao Desktop v12 Started ===");
    ESP_LOGI(TAG, "Press UP/DOWN/LEFT/RIGHT to navigate, A to enter, B to back");

    int last_btn = -1;
    uint32_t btn_changed = 0;
    int prev_stable = -1;
    while (true) {
        lv_timer_handler();

        /* Custom input: poll buttons, debounce, dispatch */
        int raw = scan_buttons();
        uint32_t now = lv_tick_get();
        if (raw != last_btn) {
            btn_changed = now;
            last_btn = raw;
        }
        if (raw >= 0 && lv_tick_elaps(btn_changed) >= BUTTON_DEBOUNCE_MS) {
            /* Detect rising edge: previous reading was different */
            if (raw != prev_stable) {
                handle_input(raw);
                prev_stable = raw;
            }
        } else if (raw < 0) {
            prev_stable = -1;
        }

        /* Battery update every 5s */
        static uint32_t last_bat = 0;
        if (lv_tick_elaps(last_bat) > 5000) {
            last_bat = lv_tick_get();
            /* Skip while BTN_A pressed */
            bool a_pressed = (gpio_get_level(BTN_A) == BUTTON_ACTIVE_LEVEL);
            if (!a_pressed && s_bat_label) {
                float v = battery_get_voltage();
                if (v >= BAT_MIN_VALID_V) {
                    int pct = battery_get_percent(v);
                    if (pct != s_last_pct) {
                        s_last_pct = pct;
                        char buf[16];
                        snprintf(buf, sizeof(buf), "%d%%", pct);
                        lv_label_set_text(s_bat_label, buf);
                    }
                }
            }
            /* Clock update - skip if label is gone */
            if (s_time_label && lv_obj_is_valid(s_time_label)) {
                time_t nowt;
                time(&nowt);
                struct tm *tm_info = localtime(&nowt);
                char tbuf[16];
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
                lv_label_set_text(s_time_label, tbuf);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}