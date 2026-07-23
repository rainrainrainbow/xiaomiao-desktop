/**
 * @file main.c
 * @brief 小喵桌面 - 模块化架构入口
 * 
 * 架构：
 * - UI框架层：ui_framework (页面栈、主题、通用组件)
 * - 应用层：app_manager, app_builtin, app_micropython
 * - 驱动层：drv_lcd, drv_button, drv_battery, drv_backlight
 * - 系统层：sys_nvs
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

// UI框架
#include "ui/ui_framework.h"

// 应用管理
#include "app/app_manager.h"

// 驱动层
#include "driver/drv_button.h"
#include "driver/drv_battery.h"
#include "driver/drv_backlight.h"

// 系统服务
#include "system/sys_nvs.h"

static const char *TAG = "MAIN";

/* ========== LCD驱动（保留在main.c，因为与LVGL紧密耦合） ========== */
#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160

#define PIN_LCD_SCLK   GPIO_NUM_18
#define PIN_LCD_MOSI   GPIO_NUM_23
#define PIN_LCD_MISO   GPIO_NUM_19
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_DC     GPIO_NUM_4

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

static esp_lcd_panel_io_handle_t s_lcd_io;
static volatile bool s_first_flush;
static bool s_display_on = false;

static void st7735_tx(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t len)
{
    esp_lcd_panel_io_tx_param(io, cmd, param, len);
}

static void st7735_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void st7735_clear_black(esp_lcd_panel_io_handle_t io)
{
    uint16_t line[LCD_H_RES * 8];
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
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2
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
        .trans_queue_depth = 10
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
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, b2, sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(d, io);
    lv_display_set_flush_cb(d, flush_cb);
    return d;
}

/* ========== 桌面页面实现 ========== */
static void desktop_page_init(void *data);
static void desktop_page_activate(void);
static void desktop_page_destroy(void);
static bool desktop_page_on_key(int key);

static const page_callbacks_t s_desktop_callbacks = {
    .init = desktop_page_init,
    .activate = desktop_page_activate,
    .destroy = desktop_page_destroy,
    .on_key = desktop_page_on_key,
};

// 桌面状态
static int s_desktop_selected = 0;
static int s_desktop_page = 0;
static int s_desktop_total_pages = 2;
static lv_obj_t *s_app_cells[8] = {0};

static void desktop_page_init(void *data)
{
    ESP_LOGI(TAG, "Desktop page init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *state = ui_state_get();
    
    // 清屏
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    // 创建状态栏
    ui_statusbar_create(scr);
    
    // 创建应用网格
    int app_count = (state->layout == 0) ? 4 : 2;
    int cols = (state->layout == 0) ? 2 : 1;
    
    lv_coord_t grid_top = 14;  // STATUS_H + 2
    lv_coord_t grid_bottom = LCD_V_RES - 10;  // LCD_V_RES - DOCK_H
    lv_coord_t grid_h = grid_bottom - grid_top;
    lv_coord_t cell_w = (LCD_H_RES - 3 * 4) / cols;
    lv_coord_t cell_h = (grid_h - 3 * 4) / 2;
    
    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    
    int start_idx = s_desktop_page * app_count;
    for (int i = 0; i < app_count && (start_idx + i) < builtin_count; i++) {
        const app_def_t *app = &builtin_apps[start_idx + i];
        int row = i / cols;
        int col = i % cols;
        
        lv_obj_t *cell = lv_obj_create(scr);
        lv_obj_set_pos(cell, 4 + col * (cell_w + 4), grid_top + 2 + row * (cell_h + 4));
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_radius(cell, 8, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(app->icon_color), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_30, 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(colors->border), 0);
        lv_obj_set_style_border_opa(cell, LV_OPA_50, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        
        // 图标
        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, app->icon_text);
        lv_obj_set_style_text_color(icon, lv_color_hex(app->icon_color), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -4);
        
        // 名称
        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, app->name);
        lv_obj_set_style_text_color(name, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -2);
        
        s_app_cells[i] = cell;
    }
    
    // 创建底部导航栏
    ui_dock_create(scr, s_desktop_total_pages, s_desktop_page);
    
    // 高亮选中项
    if (s_app_cells[s_desktop_selected]) {
        lv_obj_set_style_border_color(s_app_cells[s_desktop_selected], 
                                       lv_color_hex(colors->sel_border), 0);
        lv_obj_set_style_border_opa(s_app_cells[s_desktop_selected], LV_OPA_COVER, 0);
    }
}

static void desktop_page_activate(void)
{
    ESP_LOGI(TAG, "Desktop page activate");
}

static void desktop_page_destroy(void)
{
    ESP_LOGI(TAG, "Desktop page destroy");
    for (int i = 0; i < 8; i++) {
        s_app_cells[i] = NULL;
    }
}

static bool desktop_page_on_key(int key)
{
    ui_state_t *state = ui_state_get();
    int app_count = (state->layout == 0) ? 4 : 2;
    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    const theme_colors_t *colors = ui_theme_colors();
    
    // 取消当前高亮
    if (s_app_cells[s_desktop_selected]) {
        lv_obj_set_style_border_color(s_app_cells[s_desktop_selected], 
                                       lv_color_hex(colors->border), 0);
        lv_obj_set_style_border_opa(s_app_cells[s_desktop_selected], LV_OPA_50, 0);
    }
    
    if (key == KEY_UP) {
        if (state->layout == 0 && s_desktop_selected >= 2) {
            s_desktop_selected -= 2;
        } else if (state->layout == 1 && s_desktop_selected > 0) {
            s_desktop_selected--;
        }
    } else if (key == KEY_DOWN) {
        if (state->layout == 0 && s_desktop_selected < 2) {
            s_desktop_selected += 2;
        } else if (state->layout == 1 && s_desktop_selected < app_count - 1) {
            s_desktop_selected++;
        }
    } else if (key == KEY_LEFT) {
        if (state->layout == 0) {
            if (s_desktop_selected % 2 == 1) {
                s_desktop_selected--;
            } else if (s_desktop_page > 0) {
                s_desktop_page--;
                s_desktop_selected = app_count - 1;
                desktop_page_init(NULL);  // 重建页面
                return true;
            }
        } else {
            if (s_desktop_selected > 0) s_desktop_selected--;
        }
    } else if (key == KEY_RIGHT) {
        if (state->layout == 0) {
            if (s_desktop_selected % 2 == 0) {
                s_desktop_selected++;
            } else if (s_desktop_page < s_desktop_total_pages - 1) {
                s_desktop_page++;
                s_desktop_selected = 0;
                desktop_page_init(NULL);  // 重建页面
                return true;
            }
        } else {
            if (s_desktop_selected < app_count - 1) s_desktop_selected++;
        }
    } else if (key == KEY_A) {
        // 启动应用
        int global_idx = s_desktop_page * app_count + s_desktop_selected;
        if (global_idx < builtin_count) {
            const app_def_t *app = &builtin_apps[global_idx];
            app_manager_launch(app);
            return true;
        }
    }
    
    // 边界检查
    if (s_desktop_selected >= app_count) s_desktop_selected = app_count - 1;
    if (s_desktop_selected < 0) s_desktop_selected = 0;
    
    // 高亮新选中项
    if (s_app_cells[s_desktop_selected]) {
        lv_obj_set_style_border_color(s_app_cells[s_desktop_selected], 
                                       lv_color_hex(colors->sel_border), 0);
        lv_obj_set_style_border_opa(s_app_cells[s_desktop_selected], LV_OPA_COVER, 0);
    }
    
    return true;
}

/* ========== 主函数 ========== */
void app_main(void)
{
    return_to_loader_setup();
    
    ESP_LOGI(TAG, "=== Xiaomiao Desktop v17 (Modular) ===");
    
    // 初始化系统服务
    sys_nvs_init();
    
    // 初始化驱动
    drv_button_init();
    drv_battery_init();
    drv_backlight_init();
    
    // 加载保存的设置
    ui_state_t *state = ui_state_get();
    sys_nvs_load_settings(&state->brightness, &state->sound_on, 
                          (int*)&state->theme, &state->wifi_on, &state->layout);
    
    // 应用设置
    drv_backlight_set_brightness(state->brightness);
    ui_theme_set(state->theme);
    
    // 初始化LCD
    esp_lcd_panel_io_handle_t io = lcd_init();
    
    // 初始化LVGL
    lv_init();
    lv_display_t *disp = display_init(io);
    
    // 创建按键输入设备
    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(indev, disp);
    lv_indev_set_group(indev, group);
    
    // 注册LCD回调
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready };
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp);
    
    // 启动LVGL定时器
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lv" };
    esp_timer_handle_t tt;
    esp_timer_create(&ta, &tt);
    esp_timer_start_periodic(tt, 1000);
    
    // 初始化UI框架
    ui_stack_init();
    
    // 初始化应用管理器
    app_manager_init();
    app_builtin_register_all();
    
    // 推入桌面页面
    ui_stack_push(PAGE_DESKTOP, &s_desktop_callbacks, NULL);
    
    // 首次刷新并开启显示
    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();
    
    ESP_LOGI(TAG, "Desktop started");
    
    // 主循环
    int last_btn = -1;
    uint32_t btn_changed = 0;
    int prev_stable = -1;
    
    while (true) {
        lv_timer_handler();
        
        // 按键扫描和去抖
        int raw = drv_button_scan();
        uint32_t now = lv_tick_get();
        
        if (raw != last_btn) {
            btn_changed = now;
            last_btn = raw;
        }
        
        if (raw >= 0 && lv_tick_elaps(btn_changed) >= BUTTON_DEBOUNCE_MS) {
            if (raw != prev_stable) {
                // 分发按键事件到当前页面
                page_type_t current = ui_stack_current();
                // TODO: 获取当前页面的on_key回调并调用
                prev_stable = raw;
            }
        } else if (raw < 0) {
            prev_stable = -1;
        }
        
        // 电池更新（每5秒）
        static uint32_t last_bat = 0;
        if (lv_tick_elaps(last_bat) > 5000) {
            last_bat = lv_tick_get();
            float v = drv_battery_get_voltage();
            if (v >= BAT_MIN_VALID_V) {
                int pct = drv_battery_get_percent(v);
                // TODO: 更新状态栏电池显示
            }
            // 更新时间
            ui_statusbar_update_time();
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}