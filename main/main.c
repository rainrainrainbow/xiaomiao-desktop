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
#include <stdlib.h>
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
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "return_to_loader.h"  // 来自 components/return_to_loader

// UI框架
#include "ui/ui_framework.h"
#include "ui/event_bus.h"  // 事件总线

// 应用管理
#include "app/app_manager.h"
#include "app/app_builtin.h"
#include "app/app_micropython.h"
#include "app/bg_manager.h"

// 驱动层
#include "driver/drv_button.h"
#include "driver/drv_battery.h"
#include "driver/drv_backlight.h"
#include "driver/drv_buzzer.h"
#include "driver/drv_audio_output.h"

// 系统服务
#include "system/sys_nvs.h"

// 新架构：Poincaré MicroPython 运行时（启动时在 PSRAM 任务中预初始化，避免 main 任务栈溢出）
#include "poincare/runtime.h"

// 新架构：Ion SD 卡驱动（用于扫描 MicroPython 应用）
#include "ion/sdcard.h"

// 中文字体管理（统一中文字体获取入口，使用内置字体）
#include "fonts/lv_freetype_font.h"

// 多语言框架
#include "lang/lang.h"

// retro-core 分区挂载（FAT存储空间，用于字库/图标/音乐）
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

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
    /* 使用 PSRAM 堆分配代替栈分配（main任务栈仅8KB，栈上2560字节缓冲区风险高） */
    uint16_t *line = heap_caps_malloc(LCD_H_RES * 8 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line) {
        ESP_LOGE(TAG, "Failed to allocate clear_black buffer, using stack fallback");
        uint16_t line_stack[LCD_H_RES * 8];
        memset(line_stack, 0, sizeof(line_stack));
        const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
        st7735_tx(io, ST7735_CASET, caset, sizeof(caset));
        for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
            const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
            const uint8_t raset[] = {(uint8_t)(y>>8), (uint8_t)(y&0xFF), (uint8_t)(y2>>8), (uint8_t)(y2&0xFF)};
            st7735_tx(io, ST7735_RASET, raset, sizeof(raset));
            st7735_tx(io, ST7735_RAMWR, line_stack, (uint16_t)(y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
        }
        return;
    }
    memset(line, 0, LCD_H_RES * 8 * sizeof(uint16_t));
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    st7735_tx(io, ST7735_CASET, caset, sizeof(caset));
    for (uint16_t y = 0; y < LCD_V_RES; y += 8) {
        const uint16_t y2 = MIN((uint16_t)(y + 7), (uint16_t)(LCD_V_RES - 1));
        const uint8_t raset[] = {(uint8_t)(y>>8), (uint8_t)(y&0xFF), (uint8_t)(y2>>8), (uint8_t)(y2&0xFF)};
        st7735_tx(io, ST7735_RASET, raset, sizeof(raset));
        st7735_tx(io, ST7735_RAMWR, line, (uint16_t)(y2 - y + 1) * LCD_H_RES * sizeof(uint16_t));
    }
    free(line);
}

static void lcd_show_splash(esp_lcd_panel_io_handle_t io, uint16_t color)
{
    // 横屏状态下的显示区域设置
    const uint8_t caset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_H_RES - 1)};
    const uint8_t raset[] = {0x00, 0x00, 0x00, (uint8_t)(LCD_V_RES - 1)};
    st7735_tx(io, ST7735_CASET, caset, sizeof(caset));
    st7735_tx(io, ST7735_RASET, raset, sizeof(raset));
    
    /* 使用 PSRAM 堆分配代替栈分配（5KB+栈缓冲区对8KB main任务栈风险高） */
    uint16_t *buf = heap_caps_malloc(LCD_H_RES * 16 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGW(TAG, "Splash buf alloc failed, skipping splash");
        return;
    }
    for (int i = 0; i < LCD_H_RES * 16; i++) buf[i] = color;
    
    for (int y = 0; y < LCD_V_RES; y += 16) {
        int h = (y + 16 <= LCD_V_RES) ? 16 : (LCD_V_RES - y);
        st7735_tx(io, ST7735_RAMWR, buf, LCD_H_RES * h * sizeof(uint16_t));
    }
    free(buf);
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
        .max_transfer_sz = 8 * 1024  // 8KB 最大传输，与显示缓存一致，减少DMA压力
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
    /* 屏幕缓存使用8KB单缓冲（部分刷新模式），释放DMA内存给WiFi驱动 */
    /* 之前2×16KB=32KB DMA内存占用过高，导致WiFi驱动初始化时DMA分配失败 */
    size_t sz = 8 * 1024;
    void *b1 = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_DMA);
    if (!b1) {
        ESP_LOGE(TAG, "Failed to allocate display buffer (size=%d)!", sz);
        return d;  /* 返回创建但无缓冲的display，LVGL会报错但不会崩溃 */
    }
    lv_display_set_color_format(d, cf);
    lv_display_set_buffers(d, b1, NULL, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
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

    // 根据字体大小自适应布局
    // 字体越大，状态栏/标题栏越高，网格间距越大
    int font_px = state->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    // 状态栏高度：字体大小 + 2px padding
    lv_coord_t status_h = font_px + 2;
    if (status_h < 12) status_h = 12;  // 最小12px

    // 网格顶部：状态栏高度 + 2px间距
    lv_coord_t grid_top = status_h + 2;
    lv_coord_t grid_bottom = LCD_V_RES - DOCK_H;  // 128 - 8 = 120
    lv_coord_t grid_h = grid_bottom - grid_top;

    // 间距根据字体大小自适应缩放
    // 基础值：font=14px时 pad_x=4, pad_y=3, gap=2
    // 字体每增大1px，间距增加0.5px（取整）
    lv_coord_t pad_x = 4 + (font_px - 14) / 2;
    lv_coord_t pad_y = 3 + (font_px - 14) / 2;
    lv_coord_t gap = 2 + (font_px - 14) / 4;
    if (pad_x > 8) pad_x = 8;
    if (pad_y > 6) pad_y = 6;
    if (gap > 4) gap = 4;

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
        // 图标字体根据字体大小自适应
        // 注意：LVGL内置Montserrat只有14px可用，所以图标固定用14px
        // 但cell的flex布局会自适应大小
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

        // 名称标签（模拟器：font-size:7px, color:var(--black)）
        // 应用名为中文，使用统一中文字体（优先FreeType，回退内置）
        // 字体大小根据设置自适应：14px→14px, 16px→14px, 20px→16px, 24px→20px
        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, app_builtin_get_display_name(app->name));
        lv_obj_set_style_text_color(name, lv_color_hex(colors->text), 0);
        int name_font_size = (font_px <= 14) ? 14 : (font_px <= 16) ? 14 : (font_px <= 20) ? 16 : 20;
        lv_obj_set_style_text_font(name, lv_font_cn_get(name_font_size), 0);
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
    app_manager_get_builtin(&builtin_count);

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
    ui_statusbar_set_title(lang_get(STR_RECENTS));

    int bg_count = bg_manager_get_count();
    int rec_count = 0;
    app_manager_get_recents(&rec_count);

    // 优先显示后台运行中的应用，再显示历史记录
    int total_show = (bg_count > rec_count) ? bg_count : rec_count;
    if (total_show < 1) total_show = 1;

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    ui_state_t *st = ui_state_get();
    int item_h = st->font_size + 2;
    if (item_h < 14) item_h = 14;
    // 计算可见行数
    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / item_h;
    if (vis_rows < 1) vis_rows = 1;

    int row = 0;

    // 先显示后台运行中的应用
    for (int i = 0; i < bg_count && row < vis_rows; i++) {
        const char *app_name = bg_manager_get_name(i);
        if (!app_name) continue;
        bg_state_t state = bg_manager_get_state(i);

        lv_obj_t *row_obj = lv_obj_create(list);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_pos(row_obj, 0, row * item_h);
        lv_obj_set_size(row_obj, LCD_H_RES, item_h);
        if (row == s_recents_sel) {
            lv_obj_set_style_bg_color(row_obj, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_TRANSP, 0);
        }
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row_obj);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s", app_name);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // 状态标签
        lv_obj_t *status_lbl = lv_label_create(row_obj);
        if (state == BG_STATE_FOREGROUND) {
            lv_label_set_text(status_lbl, lang_get(STR_CURRENT));
        } else {
            lv_label_set_text(status_lbl, lang_get(STR_BACKGROUND));
        }
        lv_obj_set_style_text_color(status_lbl, lv_color_hex(
            state == BG_STATE_FOREGROUND ? 0x22C55E : colors->text_dim), 0);
        lv_obj_set_style_text_font(status_lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -4, 0);

        row++;
    }

    // 再显示历史记录（还未显示的）
    for (int i = 0; i < rec_count && row < vis_rows; i++) {
        const app_def_t *app = app_manager_get_recents_at(i);
        if (!app) break;

        // 跳过已在后台列表中显示的
        if (bg_manager_is_running(app->name)) continue;

        lv_obj_t *row_obj = lv_obj_create(list);
        lv_obj_remove_style_all(row_obj);
        lv_obj_set_pos(row_obj, 0, row * item_h);
        lv_obj_set_size(row_obj, LCD_H_RES, item_h);
        if (row == s_recents_sel) {
            lv_obj_set_style_bg_color(row_obj, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row_obj, LV_OPA_TRANSP, 0);
        }
        lv_obj_clear_flag(row_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row_obj);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", app->icon_text, app_builtin_get_display_name(app->name));
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
        row++;
    }

    if (row == 0) {
        // 没有内容
        lv_obj_t *empty_lbl = lv_label_create(scr);
        lv_label_set_text(empty_lbl, lang_get(STR_RECENTS_EMPTY));
        lv_obj_set_style_text_color(empty_lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(empty_lbl, lv_font_cn_get(ui_state_get()->font_size), 0);
        lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
    }

    s_recents_obj = list;
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
        // 获取当前选中项对应的应用
        int bg_count = bg_manager_get_count();
        int rec_count = 0;
        app_manager_get_recents(&rec_count);

        // 计算总项目数
        int total_items = 0;
        // 后台运行的应用
        for (int i = 0; i < bg_count; i++) {
            const char *name = bg_manager_get_name(i);
            if (name) total_items++;
        }
        // 历史记录中未在后台运行的
        for (int i = 0; i < rec_count; i++) {
            const app_def_t *app = app_manager_get_recents_at(i);
            if (app && !bg_manager_is_running(app->name)) total_items++;
        }

        if (total_items > 0 && s_recents_sel < total_items) {
            // 查找选中项在后台列表中的索引
            int idx = s_recents_sel;
            // 先查后台运行的应用
            for (int i = 0; i < bg_count; i++) {
                const char *name = bg_manager_get_name(i);
                if (!name) continue;
                if (idx == 0) {
                    // 重新启动该应用
                    // 找到对应的 app_def_t
                    int builtin_count;
                    const app_def_t *builtin_apps = app_manager_get_builtin(&builtin_count);
                    int py_count = 0;
                    const app_def_t *py_apps = app_manager_get_micropython(&py_count);

                    for (int j = 0; j < builtin_count; j++) {
                        if (strcmp(builtin_apps[j].name, name) == 0) {
                            app_manager_launch(&builtin_apps[j]);
                            return true;
                        }
                    }
                    for (int j = 0; j < py_count; j++) {
                        if (strcmp(py_apps[j].name, name) == 0) {
                            app_manager_launch(&py_apps[j]);
                            return true;
                        }
                    }
                    break;
                }
                idx--;
            }
            // 再查历史记录
            for (int i = 0; i < rec_count; i++) {
                const app_def_t *app = app_manager_get_recents_at(i);
                if (!app) break;
                if (bg_manager_is_running(app->name)) continue;
                if (idx == 0) {
                    app_manager_launch(app);
                    return true;
                }
                idx--;
            }
        }
        return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        // 计算总项目数
        int bg_count = bg_manager_get_count();
        int total_items = 0;
        for (int i = 0; i < bg_count; i++) {
            if (bg_manager_get_name(i)) total_items++;
        }
        int rec_count = 0;
        app_manager_get_recents(&rec_count);
        for (int i = 0; i < rec_count; i++) {
            const app_def_t *app = app_manager_get_recents_at(i);
            if (!app) break;
            if (!bg_manager_is_running(app->name)) total_items++;
        }

        if (total_items <= 0) return true;
        if (key == KEY_DOWN) s_recents_sel = (s_recents_sel + 1) % total_items;
        else s_recents_sel = (s_recents_sel - 1 + total_items) % total_items;

        // 重建页面以更新高亮
        recents_page_init(NULL);
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
#define UI_TASK_STACK_SIZE   (32 * 1024)   // 32KB 栈，优先从 PSRAM 分配（64KB在真机上分配失败）
/* (UI_TASK_STACK_DRAM, s_ui_task_tcb, s_ui_task_stack removed - no longer needed) */

static void ui_init_task(void *arg)
{
    ESP_LOGI(TAG, "UI init task started (stack=%p)", 
             (void*)arg);
    
    /*
     * 提前初始化 MicroPython 运行时（在 PSRAM 64KB 栈中执行）。
     * 后续 main 任务中 poincare_runtime_init 因幂等性（s_initialized=true）直接返回。
     */
    if (!poincare_runtime_init(0)) {
        ESP_LOGE(TAG, "Failed to pre-init MicroPython runtime");
    } else {
        ESP_LOGI(TAG, "MicroPython runtime pre-initialized successfully");
    }
    
    // 挂载 retro-core FAT 分区（用于存放系统字库、图标、内置音乐等）
    // 分区表：retro-core, data, fat, 0x2C0000, 0x140000 (1.25MB)
    ESP_LOGI(TAG, "Mounting retro-core partition at /flash...");
    const esp_partition_t *retro_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "retro-core");
    // 如果精确子类型匹配失败，回退到按标签名模糊查找
    if (!retro_part) {
        ESP_LOGW(TAG, "retro-core not found with SUBTYPE_DATA_FAT, trying SUBTYPE_ANY...");
        retro_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "retro-core");
    }
    if (retro_part) {
        ESP_LOGI(TAG, "retro-core partition found: offset=0x%08X, size=%lu KB",
                 (unsigned int)retro_part->address,
                 (unsigned long)(retro_part->size / 1024));
        
        // 配置 FAT 挂载
        static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = true,
            .max_files = 8,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        };
        esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl("/flash", "retro-core",
                                                          &mount_cfg, &s_wl_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "retro-core partition mounted at /flash");
        } else {
            ESP_LOGW(TAG, "Failed to mount retro-core: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "retro-core partition not found!");
    }
    
    // 推入桌面页面
    ui_stack_push(PAGE_DESKTOP, &s_desktop_callbacks, NULL);
    
    // 尝试初始化 SD 卡并扫描 MicroPython 应用
    // 注意：SD 卡与 LCD 共享 SPI2 总线，需要确保 LCD 已初始化完毕
    ESP_LOGI(TAG, "Initializing SD card and scanning MicroPython apps...");
    bool sdcard_ok = ion_sdcard_init("/sdcard");
    if (sdcard_ok) {
        int app_count = app_manager_scan_sdcard();
        ESP_LOGI(TAG, "SD card ready, found %d MicroPython apps", app_count);
    }
    
    // 根据字库来源设置初始化字体
    ui_state_t *state = ui_state_get();
    if (state->font_source == 0) {
        /* FreeType 模式：优先加载用户自定义字体（NVS 中的路径索引） */
        int font_path_idx = sys_nvs_load_font_path();
        bool font_loaded = false;
        if (font_path_idx > 0) {
            /* 用户选择了某个字体文件（1=扫描列表第1个, ...），扫描并加载对应路径 */
            char paths[16][128];
            int n = lv_freetype_font_scan(paths, 16);
            if (font_path_idx - 1 < n) {
                if (lv_freetype_font_load_path(paths[font_path_idx - 1]) == LV_RESULT_OK) {
                    ESP_LOGI(TAG, "FreeType font loaded from user-selected path: %s",
                             paths[font_path_idx - 1]);
                    font_loaded = true;
                }
            }
        }
        if (!font_loaded) {
            /* 回退到默认路径 */
            if (sdcard_ok) {
                if (lv_freetype_font_init() == LV_RESULT_OK) {
                    ESP_LOGI(TAG, "FreeType font engine initialized from SD card");
                } else {
                    ESP_LOGW(TAG, "FreeType font init failed, falling back to English UI");
                }
            } else {
                /* SD卡不可用时，尝试从其他路径加载字体 */
                if (lv_freetype_font_init() == LV_RESULT_OK) {
                    ESP_LOGI(TAG, "FreeType font engine initialized (fallback path)");
                } else {
                    ESP_LOGW(TAG, "FreeType font init failed (no SD card), falling back to English UI");
                }
            }
        }
    } else {
        /* 内置模式：不加载 FreeType 字体。
         * 语言由用户选择（lang_get 在 FreeType 未就绪时会自动降级英文） */
        ESP_LOGI(TAG, "Font source set to built-in, FreeType not loaded");
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
    
    // 初始化网络接口（WiFi用，只需一次）
    esp_netif_init();
    esp_event_loop_create_default();
    
    // 重要：先初始化按键，再初始化电池（因为GPIO34共享）
    drv_button_init();
    drv_backlight_init();
    drv_battery_init();  // 电池在按键之后，避免覆盖GPIO34配置
    drv_buzzer_init();   // 初始化蜂鸣器（GPIO14，LEDC PWM）
    audio_output_init(); // 初始化音频输出抽象层（自动检测并选择最佳设备）
    
    // 启动按键任务（独立任务，5ms扫描周期）
    xTaskCreate(drv_button_task, "btn_task", 2048, NULL, 10, NULL);
    ESP_LOGI(TAG, "Button task created");
    
    // 加载保存的设置
    ui_state_t *state = ui_state_get();
    sys_nvs_load_settings(&state->brightness, &state->volume, &state->sound_on, 
                      (int*)&state->theme, &state->wifi_on, &state->layout, &state->font_size);
    state->font_source = sys_nvs_load_font_source();
    // 加载用户选择的语言（0=中文, 1=English），持久化后重启生效
    int saved_lang = sys_nvs_load_language();
    if (saved_lang == LANG_EN) lang_set(LANG_EN);
    else lang_set(LANG_ZH);
    
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
    
    // 创建 UI 初始化任务（使用 xTaskCreate，FreeRTOS 自动从 PSRAM 分配栈）
    // CONFIG_FREERTOS_TASK_STACK_ALLOCATION_FROM_SPIRAM_FIRST=y 时：
    // 1. FreeRTOS 自动优先从 PSRAM 分配栈（64KB）
    // 2. 如果 PSRAM 分配失败，自动回退到内部 DRAM
    // 3. 任务完成后保持空闲循环，避免栈被释放
    TaskHandle_t ui_task_handle = NULL;
    BaseType_t ret = xTaskCreate(
        ui_init_task, "ui_init", UI_TASK_STACK_SIZE / sizeof(StackType_t),
        NULL, 5, &ui_task_handle);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "ui_init_task created via xTaskCreate (stack_size=%d)", 
                 UI_TASK_STACK_SIZE);
    } else {
        ESP_LOGE(TAG, "xTaskCreate failed for ui_init_task (ret=%d)", ret);
    }
    
    ESP_LOGI(TAG, "Main loop started - waiting for button events...");
    
    // 屏幕超时状态
    static bool s_screen_sleeping = false;
    static uint32_t s_last_activity = 0;
    
    // 主循环 - 从事件队列获取按键
    while (true) {
        lv_timer_handler();
        
        // 音频后端热插拔检测（蓝牙A2DP设备连接/断开时自动切换）
        audio_output_poll();
        
        // Python 应用帧刷新（canvas 承接 framebuffer 的脏标志消费）
        app_micropython_on_tick();
        
        // 从队列获取按键事件（非阻塞）
        btn_event_t btn_evt;
        if (drv_button_get_event(&btn_evt)) {
            int btn_event = btn_evt.key;
            bool is_long = btn_evt.is_long_press;
            ESP_LOGI(TAG, "KEY EVENT: idx=%d long=%d (UP=0,DOWN=1,LEFT=2,RIGHT=3,A=4,B=5)", 
                     btn_event, is_long);
            
            // 如果屏幕处于休眠状态，按键唤醒屏幕
            if (s_screen_sleeping) {
                s_screen_sleeping = false;
                drv_backlight_set_brightness(state->brightness);
                lcd_display_on();
                s_last_activity = lv_tick_get();
                ESP_LOGI(TAG, "Screen woken up by key press");
                continue;
            }
            
            // 记录按键活动时间（重置超时计时器）
            s_last_activity = lv_tick_get();
            
            // 全局处理：长按B → 进入最近任务页面
            if (is_long && btn_event == BTN_IDX_B) {
                // 挂起当前前台应用到后台
                bg_manager_suspend_current();
                ui_stack_push(PAGE_RECENTS, &s_recents_callbacks, NULL);
                continue;
            }
            
            // 全局处理：长按A → 返回桌面主页（挂起当前应用到后台）
            if (is_long && btn_event == BTN_IDX_A) {
                bg_manager_suspend_current();
                ui_stack_back_home();
                continue;
            }
            
            // 分发按键事件到当前页面的 on_key 回调
            const page_callbacks_t *cbs = ui_stack_current_callbacks();
            if (cbs && cbs->on_key) {
                bool handled = cbs->on_key(btn_event);
                if (!handled) {
                    // 全局兜底：B键=返回上一级（挂起当前应用到后台）
                    if (btn_event == BTN_IDX_B && ui_stack_depth() > 1) {
                        bg_manager_suspend_current();
                        ui_stack_pop();
                    }
                }
            } else {
                // 无回调时的兜底：B键=返回（挂起当前应用到后台）
                if (btn_event == BTN_IDX_B && ui_stack_depth() > 1) {
                    bg_manager_suspend_current();
                    ui_stack_pop();
                }
            }
        }
        
        // 屏幕超时检测（每100ms检查一次）
        if (!s_screen_sleeping && state->sleep_timeout > 0) {
            uint32_t elapsed = lv_tick_elaps(s_last_activity);
            if (elapsed > (uint32_t)state->sleep_timeout * 1000) {
                s_screen_sleeping = true;
                drv_backlight_set_brightness(0);  // 关闭背光
                ESP_LOGI(TAG, "Screen sleep: timeout=%ds elapsed=%lums", 
                         state->sleep_timeout, (unsigned long)elapsed);
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