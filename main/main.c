/**
 * @file main.c
 * @brief 小喵桌面 - 模块化架构入口
 *
 * 架构：
 * - UI框架层：ui_framework (页面栈、主题、通用组件)
 * - 应用层：app_manager, app_builtin, app_micropython
 * - 驱动层：drv_lcd, drv_button, drv_battery, drv_backlight
 * - 系统层：sys_nvs
 *
 * v20 (2026-07-23): 修复背光黑屏问题
 *   - 背光初始化时直接设置为100%亮度
 *   - 修复PWM极性反转（duty越大越暗）
 *   - 确保屏幕启动时可见
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
#include "ui/event_bus.h"  // 事件总线

// 应用管理
#include "app/app_manager.h"

// 驱动层
#include "driver/drv_button.h"
#include "driver/drv_battery.h"
#include "driver/drv_backlight.h"

// 系统服务
#include "system/sys_nvs.h"

// 新架构：Poincaré MicroPython 运行时（启动时在 PSRAM 任务中预初始化，避免 main 任务栈溢出）
#include "poincare/runtime.h"

// 新架构：Ion SD 卡驱动（用于扫描 MicroPython 应用）
#include "ion/sdcard.h"

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

static void lcd_show_splash(esp_lcd_panel_io_handle_t io, uint16_t color)
{
    // 横屏状态下的显示区域设置
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    const uint8_t raset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_V_RES - 1)};
    st7735_tx(io, ST7735_CASET, caset, sizeof(caset));
    st7735_tx(io, ST7735_RASET, raset, sizeof(raset));
    
    // 填充整屏纯色（分块发送避免大缓冲）
    uint16_t buf[LCD_H_RES * 16];
    for (int i = 0; i < LCD_H_RES * 16; i++) buf[i] = color;
    
    for (int y = 0; y < LCD_V_RES; y += 16) {
        int h = (y + 16 <= LCD_V_RES) ? 16 : (LCD_V_RES - y);
        st7735_tx(io, ST7735_RAMWR, buf, LCD_H_RES * h * sizeof(uint16_t));
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

/* ========== 最近任务（Recents）页面实现 ========== */
static void recents_page_init(void *data);
static void recents_page_destroy(void);
static bool recents_page_on_key(int key);

static const page_callbacks_t s_recents_callbacks = {
    .init = recents_page_init,
    .destroy = recents_page_destroy,
    .on_key = recents_page_on_key,
};

// 桌面状态
// 模拟器风格：3列×2行 = 每页 6 个应用
static int s_desktop_selected = 0;
static int s_desktop_page = 0;
static int s_desktop_total_pages = 1;
static lv_obj_t *s_app_cells[6] = {0};

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

    // 回到桌面时清除当前应用名（状态栏显示"XiaoMiaoOS"）
    // 必须在 ui_statusbar_create 之前清除，因为 create 中会读取 app_manager_get_current_name()
    app_manager_clear_current();
    ui_statusbar_create(scr);

    // 布局模式：0=6应用/页（3列×2行），1=2应用/页（2列×1行）
    // 从设置读取布局，让设置应用中的"布局"选项真正生效
    int cols, rows;
    if (state->layout == 1) {
        cols = 2;
        rows = 1;
    } else {
        cols = 3;
        rows = 2;
    }
    int app_count = cols * rows;

    lv_coord_t grid_top = 14;  // STATUS_H(12) + 2
    lv_coord_t grid_bottom = LCD_V_RES - DOCK_H;  // 128 - 8 = 120
    lv_coord_t grid_h = grid_bottom - grid_top;    // 106
    // 模拟器：padding: 3px 4px, gap: 2px
    lv_coord_t pad_x = 4;
    lv_coord_t pad_y = 3;
    lv_coord_t gap = 2;
    lv_coord_t cell_w = (LCD_H_RES - 2 * pad_x - (cols - 1) * gap) / cols;
    lv_coord_t cell_h = (grid_h - 2 * pad_y - (rows - 1) * gap) / rows;

    // 更新总页数
    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    
    // 合并内置应用和MicroPython应用
    int py_count = 0;
    const app_def_t *py_apps = app_manager_get_micropython(&py_count);
    int total_apps = builtin_count + py_count;
    
    s_desktop_total_pages = (total_apps + app_count - 1) / app_count;
    if (s_desktop_total_pages < 1) s_desktop_total_pages = 1;
    if (s_desktop_page >= s_desktop_total_pages) s_desktop_page = s_desktop_total_pages - 1;
    if (s_desktop_selected >= app_count) s_desktop_selected = 0;

    int start_idx = s_desktop_page * app_count;
    for (int i = 0; i < app_count; i++) {
        int global_idx = start_idx + i;
        if (global_idx >= total_apps) break;
        
        // 先显示内置应用，再显示MicroPython应用
        const app_def_t *app = NULL;
        if (global_idx < builtin_count) {
            app = &builtin_apps[global_idx];
        } else {
            app = &py_apps[global_idx - builtin_count];
        }
        int row = i / cols;
        int col = i % cols;

        // 模拟器：.icon{display:flex;flex-direction:column;align-items:center;justify-content:center;border-radius:4px;gap:1px}
        lv_obj_t *cell = lv_obj_create(scr);
        lv_obj_set_pos(cell, pad_x + col * (cell_w + gap), grid_top + pad_y + row * (cell_h + gap));
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_set_style_bg_color(cell, lv_color_hex(0), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);  // 默认透明背景
        lv_obj_set_style_border_width(cell, 0, 0);        // 无边框
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        // flex 居中布局
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, 
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 1, 0);

        // 图标 glyph（模拟器：font-size:20px, line-height:22px）
        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, app->icon_text);
        lv_obj_set_style_text_color(icon, lv_color_hex(app->icon_color), 0);
        // 使用 LVGL 内置较大字体——montserrat_20 或 16（如果可用）
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

        // 名称标签（模拟器：font-size:7px, color:var(--black)）
        // 应用名为中文，使用 CJK 14px 字体（LVGL 9.5 无更小 CJK 字体）
        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, app->name);
        lv_obj_set_style_text_color(name, lv_color_hex(colors->text), 0);
        LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
        lv_obj_set_style_text_font(name, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

        s_app_cells[i] = cell;
    }

    // 创建底部导航栏
    ui_dock_create(scr, s_desktop_total_pages, s_desktop_page);

    // 高亮选中项（模拟器：棕色背景 + 奶油色文字）
    if (s_app_cells[s_desktop_selected]) {
        ui_desktop_cell_set_selected(s_app_cells[s_desktop_selected], true);
    }
}

static void desktop_page_activate(void)
{
    ESP_LOGI(TAG, "Desktop page activate");
}

static void desktop_page_destroy(void)
{
    ESP_LOGI(TAG, "Desktop page destroy");
    for (int i = 0; i < 6; i++) {  /* 3 cols × 2 rows */
        s_app_cells[i] = NULL;
    }
}

static bool desktop_page_on_key(int key)
{
    // B 键在桌面页：返回true，不执行返回（避免弹出桌面页导致崩溃）
    // 长按B进入最近任务的逻辑在主循环中由按键驱动事件处理
    if (key == KEY_B) {
        return true;
    }

    ui_state_t *state = ui_state_get();
    // 与 desktop_page_init 保持一致的布局
    int cols, rows;
    if (state->layout == 1) {
        cols = 2;
        rows = 1;
    } else {
        cols = 3;
        rows = 2;
    }
    const int app_count = cols * rows;
    int builtin_count;
    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
    const theme_colors_t *colors = ui_theme_colors();

    /*
     * 防御性保护：
     * 在极端情况下（例如页面切换生命周期存在遗漏），s_app_cells[] 中的指针
     * 可能指向已被 LVGL 销毁的对象（悬空指针）。lv_obj_is_valid() 会校验
     * 对象是否仍位于 LVGL 对象池中，避免对已释放内存解引用导致
     * LoadProhibited 崩溃（曾出现 EXCVADDR=0xfffffffb）。
     */
    if (s_app_cells[s_desktop_selected] &&
        lv_obj_is_valid(s_app_cells[s_desktop_selected])) {
        ui_desktop_cell_set_selected(s_app_cells[s_desktop_selected], false);
    }

    if (key == KEY_UP) {
        if (s_desktop_selected >= cols) {
            s_desktop_selected -= cols;
        }
    } else if (key == KEY_DOWN) {
        if (s_desktop_selected + cols < app_count) {
            s_desktop_selected += cols;
        }
    } else if (key == KEY_LEFT) {
        if (s_desktop_selected % cols > 0) {
            s_desktop_selected--;
        } else if (s_desktop_page > 0) {
            s_desktop_page--;
            s_desktop_selected = app_count - 1;
            desktop_page_init(NULL);  // 重建页面
            return true;
        }
    } else if (key == KEY_RIGHT) {
        if (s_desktop_selected % cols < cols - 1 &&
            s_desktop_selected < app_count - 1) {
            s_desktop_selected++;
        } else if (s_desktop_selected % cols == cols - 1 &&
                   s_desktop_page < s_desktop_total_pages - 1) {
            s_desktop_page++;
            s_desktop_selected = 0;
            desktop_page_init(NULL);  // 重建页面
            return true;
        }
    } else if (key == KEY_A) {
        // 启动应用
        int global_idx = s_desktop_page * app_count + s_desktop_selected;
        int builtin_count;
        const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
        int py_count = 0;
        const app_def_t *py_apps = app_manager_get_micropython(&py_count);
        int total_apps = builtin_count + py_count;
        
        if (global_idx < total_apps) {
            const app_def_t *app = NULL;
            if (global_idx < builtin_count) {
                app = &builtin_apps[global_idx];
            } else {
                app = &py_apps[global_idx - builtin_count];
            }
            app_manager_launch(app);
            return true;
        }
    }

    // 边界检查
    if (s_desktop_selected >= app_count) s_desktop_selected = app_count - 1;
    if (s_desktop_selected < 0) s_desktop_selected = 0;

    // 高亮新选中项（模拟器：棕色背景）
    if (s_app_cells[s_desktop_selected] &&
        lv_obj_is_valid(s_app_cells[s_desktop_selected])) {
        ui_desktop_cell_set_selected(s_app_cells[s_desktop_selected], true);
    }

    return true;
}

/* ========== 最近任务（Recents）页面实现 ========== */
static lv_obj_t *s_recents_obj = NULL;
static int s_recents_sel = 0;

static void recents_page_init(void *data)
{
    ESP_LOGI(TAG, "Recents page init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // 状态栏
    ui_statusbar_create(scr);
    // 标题栏（中文，CJK 字体）
    ui_titlebar_create(scr, 14, "最近任务");

    int rec_count = 0;
    app_manager_get_recents(&rec_count);
    if (rec_count > 0) {
        lv_obj_t *list = lv_obj_create(scr);
        lv_obj_remove_style_all(list);
        lv_obj_set_pos(list, 0, 26);
        lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - 26 - DOCK_H);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

        int max_show = (rec_count < 6) ? rec_count : 6;
        int item_h = 14;
        for (int i = 0; i < max_show; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_remove_style_all(row);
            lv_obj_set_pos(row, 0, i * item_h);
            lv_obj_set_size(row, LCD_H_RES, item_h);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            const app_def_t *app = app_manager_get_recents_at(i);
            if (!app) break;
            lv_obj_t *lbl = lv_label_create(row);
            char buf[40];
            snprintf(buf, sizeof(buf), "%s %s", app->icon_text, app->name);
            lv_label_set_text(lbl, buf);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            // 应用名为中文，使用 CJK 字体
            LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
            lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        }
        s_recents_obj = list;
    } else {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "暂无最近任务");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x1B1713), 0);
        LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
        lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    s_recents_sel = 0;
    ui_dock_create(scr, 1, 0);
}

static void recents_page_destroy(void)
{
    ESP_LOGI(TAG, "Recents page destroy");
    s_recents_obj = NULL;
}

static bool recents_page_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    if (key == KEY_A) {
        int rec_count = 0;
        app_manager_get_recents(&rec_count);
        if (rec_count > 0 && s_recents_sel < rec_count) {
            const app_def_t *app = app_manager_get_recents_at(s_recents_sel);
            if (app) {
                app_manager_launch(app);
                return true;
            }
        }
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        int rec_count = 0;
        app_manager_get_recents(&rec_count);
        if (rec_count <= 0) return true;
        // 简单高亮切换
        if (key == KEY_DOWN) s_recents_sel = (s_recents_sel + 1) % rec_count;
        else s_recents_sel = (s_recents_sel - 1 + rec_count) % rec_count;
        return true;
    }
    return true;
}

/* ========== MicroPython 运行时初始化任务（独立任务，PSRAM 栈，避免 main 任务栈溢出） ========== */

/*
 * MicroPython 运行时初始化（mp_init + gc_init + machine_init + machine_pins_init）
 * 需要大量栈空间（>16KB），而 main 任务栈只有 16KB 内部 DRAM，
 * 在 main 任务中初始化 MicroPython 会栈溢出导致 StoreProhibited 崩溃。
 * 
 * 使用 xTaskCreateStatic + heap_caps_malloc 从 PSRAM 手动分配栈：
 * 1. 先在 PSRAM 中分配栈空间（64KB）
 * 2. 如果 PSRAM 分配失败，回退到内部 DRAM 分配（32KB）
 * 3. 使用 xTaskCreateStatic 绑定栈和 TCB
 * 4. 任务完成后保持空闲循环，避免栈被释放
 * 
 * 注意：xTaskCreate 动态分配（依赖 CONFIG_FREERTOS_TASK_STACK_ALLOCATION_FROM_SPIRAM_FIRST）
 * 在真机上可能因 PSRAM 碎片化或配置问题回退到 DRAM，导致 64KB 栈在 DRAM 中不稳定。
 * 因此采用手动分配方式，确保栈从 PSRAM 分配且检查分配成功。
 */
#define UI_TASK_STACK_SIZE   (64 * 1024)   // 64KB 栈，优先从 PSRAM 分配
#define UI_TASK_STACK_DRAM   (32 * 1024)   // 32KB 回退栈，从 DRAM 分配

/* 静态 TCB 和栈（用于 xTaskCreateStatic） */
static StaticTask_t s_ui_task_tcb;
static void *s_ui_task_stack = NULL;

static void ui_init_task(void *arg)
{
    ESP_LOGI(TAG, "UI init task started (stack=%p, size=%d)", 
             s_ui_task_stack, 
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? UI_TASK_STACK_SIZE : UI_TASK_STACK_DRAM);
    
    /*
     * 提前初始化 MicroPython 运行时（在 PSRAM 64KB 栈中执行）。
     * 后续 main 任务中 poincare_runtime_init 因幂等性（s_initialized=true）直接返回。
     */
    if (!poincare_runtime_init(0)) {
        ESP_LOGE(TAG, "Failed to pre-init MicroPython runtime");
    } else {
        ESP_LOGI(TAG, "MicroPython runtime pre-initialized successfully");
    }
    
    // 推入桌面页面
    ui_stack_push(PAGE_DESKTOP, &s_desktop_callbacks, NULL);
    
    // 尝试初始化 SD 卡并扫描 MicroPython 应用
    // 注意：SD 卡与 LCD 共享 SPI2 总线，需要确保 LCD 已初始化完毕
    ESP_LOGI(TAG, "Initializing SD card and scanning MicroPython apps...");
    if (ion_sdcard_init("/sdcard")) {
        int app_count = app_manager_scan_sdcard();
        ESP_LOGI(TAG, "SD card ready, found %d MicroPython apps", app_count);
    } else {
        ESP_LOGW(TAG, "SD card not available (no card inserted?)");
    }
    
    ESP_LOGI(TAG, "Desktop page pushed successfully");
    
    // 任务完成后不删除，进入空闲循环（保持 LVGL 对象存活）
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ========== 主函数 ========== */
void app_main(void)
{
    return_to_loader_setup();
    
    ESP_LOGI(TAG, "=== Xiaomiao Desktop v20 (Backlight Fix) ===");
    
    // 初始化系统服务
    sys_nvs_init();
    
    // 重要：先初始化按键，再初始化电池（因为GPIO34共享）
    drv_button_init();
    drv_backlight_init();
    drv_battery_init();  // 电池在按键之后，避免覆盖GPIO34配置
    
    // 启动按键任务（独立任务，5ms扫描周期）
    xTaskCreate(drv_button_task, "btn_task", 2048, NULL, 10, NULL);
    ESP_LOGI(TAG, "Button task created");
    
    // 加载保存的设置
    ui_state_t *state = ui_state_get();
    sys_nvs_load_settings(&state->brightness, &state->sound_on, 
                          (int*)&state->theme, &state->wifi_on, &state->layout);
    
    // 应用设置
    drv_backlight_set_brightness(state->brightness);
    ui_theme_set(state->theme);
    
    // 初始化LCD
    esp_lcd_panel_io_handle_t io = lcd_init();
    
    // 启动画面：直接填充蓝色确认LCD工作正常
    lcd_show_splash(io, 0x001F);  // 蓝色纯色
    ESP_LOGI(TAG, "Splash screen shown (blue)");
    
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
    
    // 初始化事件总线
    event_bus_init();
    
    // 初始化UI框架
    ui_stack_init();
    
    // 初始化应用管理器
    app_manager_init();
    app_builtin_register_all();
    
    // 首次刷新并开启显示（先显示黑屏）
    s_first_flush = false;
    lv_refr_now(NULL);
    for (int i = 0; i < 100 && !s_first_flush; i++)
        vTaskDelay(pdMS_TO_TICKS(1));
    lcd_display_on();
    
    ESP_LOGI(TAG, "LVGL initialized, starting UI task...");
    
    // 创建 UI 初始化任务（手动分配 PSRAM 栈，64KB）
    // 使用 xTaskCreateStatic + heap_caps_malloc 确保栈从 PSRAM 分配
    // 先尝试在 PSRAM 分配 64KB，失败则回退到 DRAM 32KB
    size_t stack_size = UI_TASK_STACK_SIZE;
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    s_ui_task_stack = heap_caps_malloc(stack_size, caps);
    if (s_ui_task_stack == NULL) {
        ESP_LOGW(TAG, "PSRAM stack allocation failed (%d bytes), trying DRAM...", stack_size);
        stack_size = UI_TASK_STACK_DRAM;
        s_ui_task_stack = malloc(stack_size);
        if (s_ui_task_stack == NULL) {
            ESP_LOGE(TAG, "Failed to allocate UI task stack even from DRAM");
        } else {
            ESP_LOGI(TAG, "UI task stack allocated from DRAM (%d bytes)", stack_size);
        }
    } else {
        ESP_LOGI(TAG, "UI task stack allocated from PSRAM (%d bytes)", stack_size);
    }
    
    if (s_ui_task_stack) {
        TaskHandle_t ui_task_handle = xTaskCreateStatic(
            ui_init_task, "ui_init", stack_size / sizeof(StackType_t),
            NULL, 5, (StackType_t *)s_ui_task_stack, &s_ui_task_tcb);
        if (ui_task_handle) {
            ESP_LOGI(TAG, "ui_init_task created (stack=%p, size=%d)", 
                     s_ui_task_stack, stack_size);
        } else {
            ESP_LOGE(TAG, "xTaskCreateStatic failed for ui_init_task");
        }
    }
    
    ESP_LOGI(TAG, "Main loop started - waiting for button events...");
    
    // 主循环 - 从事件队列获取按键
    while (true) {
        lv_timer_handler();
        
        // 从队列获取按键事件（非阻塞）
        btn_event_t btn_evt;
        if (drv_button_get_event(&btn_evt)) {
            int btn_event = btn_evt.key;
            bool is_long = btn_evt.is_long_press;
            ESP_LOGI(TAG, "KEY EVENT: idx=%d long=%d (UP=0,DOWN=1,LEFT=2,RIGHT=3,A=4,B=5)", 
                     btn_event, is_long);
            
            // 全局处理：长按B → 进入最近任务页面
            if (is_long && btn_event == BTN_IDX_B) {
                ui_stack_push(PAGE_RECENTS, &s_recents_callbacks, NULL);
                continue;
            }
            
            // 全局处理：长按A → 返回桌面主页（使用 v59 新 API）
            if (is_long && btn_event == BTN_IDX_A) {
                ui_stack_back_home();
                continue;
            }
            
            // 分发按键事件到当前页面的 on_key 回调
            const page_callbacks_t *cbs = ui_stack_current_callbacks();
            if (cbs && cbs->on_key) {
                bool handled = cbs->on_key(btn_event);
                if (!handled) {
                    // 全局兜底：B键=返回上一级（仅当栈深>1时，避免弹出桌面导致崩溃）
                    if (btn_event == BTN_IDX_B && ui_stack_depth() > 1) {
                        ui_stack_pop();
                    }
                }
            } else {
                // 无回调时的兜底：B键=返回（仅当栈深>1时）
                if (btn_event == BTN_IDX_B && ui_stack_depth() > 1) {
                    ui_stack_pop();
                }
            }
        }
        
        // 电池更新（每5秒）
        static uint32_t last_bat = 0;
        if (lv_tick_elaps(last_bat) > 5000) {
            last_bat = lv_tick_get();
            float v = drv_battery_get_voltage();
            if (v >= BAT_MIN_VALID_V) {
                int pct = drv_battery_get_percent(v);
                // 更新状态栏电池显示
                if (state->bat_label && lv_obj_is_valid(state->bat_label)) {
                    char bbuf[8];
                    snprintf(bbuf, sizeof(bbuf), "%d%%", pct);
                    lv_label_set_text(state->bat_label, bbuf);
                }
            }
            // 更新时间
            ui_statusbar_update_time();
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}