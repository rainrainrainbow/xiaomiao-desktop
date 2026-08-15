/**
 * @file ion/display.c
 * @brief Ion - Hardware Abstraction Layer: Display Implementation
 * 
 * ST7735 TFT 显示屏驱动实现（160x128 横屏）。
 * 基于 ESP-IDF SPI 驱动，支持 RGB565 颜色格式。
 */

#include "ion/display.h"
#include "ion/spi_mutex.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ION_DISPLAY";

/* ========== 硬件引脚定义 ========== */
#define PIN_NUM_MOSI    GPIO_NUM_21  /* SPI MOSI */
#define PIN_NUM_SCLK    GPIO_NUM_18  /* SPI Clock */
#define PIN_NUM_CS      GPIO_NUM_5   /* SPI Chip Select */
#define PIN_NUM_DC      GPIO_NUM_19  /* Data/Command */
#define PIN_NUM_RST     GPIO_NUM_23  /* Reset */
#define PIN_NUM_BL      GPIO_NUM_0   /* Backlight（PWM 控制）*/

/* ========== ST7735 命令 ========== */
#define ST7735_NOP      0x00
#define ST7735_SWRESET  0x01
#define ST7735_SLPIN    0x10
#define ST7735_SLPOUT   0x11
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_DISSET5  0xB6
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_INVOFF   0x20
#define ST7735_INVON    0x21
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_RAMWR    0x2C
#define ST7735_RAMRD    0x2E
#define ST7735_DISPON   0x29
#define ST7735_DISPOFF  0x28

/* MADCTL 参数 */
#define MADCTL_MY       0x80  /* Row address order */
#define MADCTL_MX       0x40  /* Column address order */
#define MADCTL_MV       0x20  /* Row/Column exchange */
#define MADCTL_ML       0x10  /* Vertical refresh order */
#define MADCTL_BGR      0x08  /* BGR color order */
#define MADCTL_MH       0x04  /* Horizontal refresh order */

/* ========== 颜色格式 ========== */
#define ST7735_COLOR_MODE_16BIT 0x05  /* 16-bit color (RGB565) */
#define ST7735_COLOR_MODE_18BIT 0x06  /* 18-bit color (RGB666) */

/* ========== SPI 设备 ========== */
static spi_device_handle_t s_spi_handle = NULL;

/* ========== 帧缓冲区（PSRAM） ========== */
static ion_color_t *s_framebuffer = NULL;
#define FB_SIZE (ION_DISPLAY_WIDTH * ION_DISPLAY_HEIGHT)  /* 160*128 = 20480 pixels */

/* ========== 背光亮度 ========== */
static uint8_t s_brightness = 50;
#define BRIGHTNESS_MAX 100

/* ========== 静态函数声明 ========== */
static void st7735_write_cmd(uint8_t cmd);
static void st7735_write_data(uint8_t data);
static void st7735_write_data16(uint16_t data);
static void st7735_write_data_bulk(const uint8_t *data, size_t len);
static void st7735_set_address_window(int x0, int y0, int x1, int y1);
static void st7735_hardware_reset(void);
static void st7735_init_sequence(void);

/* ========== 底层 SPI 通信 ========== */

static void st7735_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,  /* DC=0: Command */
    };
    gpio_set_level(PIN_NUM_DC, 0);  /* Command mode */
    spi_device_transmit(s_spi_handle, &t);
}

static void st7735_write_data(uint8_t data)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
        .user = (void *)1,  /* DC=1: Data */
    };
    gpio_set_level(PIN_NUM_DC, 1);  /* Data mode */
    spi_device_transmit(s_spi_handle, &t);
}

static void st7735_write_data16(uint16_t data)
{
    uint8_t buf[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = buf,
        .user = (void *)1,
    };
    gpio_set_level(PIN_NUM_DC, 1);
    spi_device_transmit(s_spi_handle, &t);
}

static void st7735_write_data_bulk(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
        .user = (void *)1,
    };
    gpio_set_level(PIN_NUM_DC, 1);
    spi_device_transmit(s_spi_handle, &t);
}

/* ========== 设置地址窗口 ========== */

static void st7735_set_address_window(int x0, int y0, int x1, int y1)
{
    st7735_write_cmd(ST7735_CASET);  /* Column address set */
    st7735_write_data16((uint16_t)x0);
    st7735_write_data16((uint16_t)x1);

    st7735_write_cmd(ST7735_RASET);  /* Row address set */
    st7735_write_data16((uint16_t)y0);
    st7735_write_data16((uint16_t)y1);
}

/* ========== 硬件复位 ========== */

static void st7735_hardware_reset(void)
{
    gpio_set_level(PIN_NUM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_NUM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ========== 初始化序列 ========== */

static void st7735_init_sequence(void)
{
    /* 软件复位 */
    st7735_write_cmd(ST7735_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* 退出睡眠模式 */
    st7735_write_cmd(ST7735_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 帧率控制 */
    st7735_write_cmd(ST7735_FRMCTR1);
    st7735_write_data(0x01);  /* 帧率 = 130Hz(Linux) / 118Hz(OS) */
    st7735_write_data(0x2C);
    st7735_write_data(0x2D);

    /* 电源控制 */
    st7735_write_cmd(ST7735_PWCTR1);
    st7735_write_data(0xA2);
    st7735_write_data(0x02);
    st7735_write_data(0x84);

    st7735_write_cmd(ST7735_PWCTR2);
    st7735_write_data(0xC5);

    st7735_write_cmd(ST7735_PWCTR3);
    st7735_write_data(0x0A);
    st7735_write_data(0x00);

    st7735_write_cmd(ST7735_PWCTR4);
    st7735_write_data(0x8A);
    st7735_write_data(0x2A);

    st7735_write_cmd(ST7735_PWCTR5);
    st7735_write_data(0x8A);
    st7735_write_data(0xEE);

    /* 电压控制 */
    st7735_write_cmd(ST7735_VMCTR1);
    st7735_write_data(0x0E);

    /* 显示反转控制 */
    st7735_write_cmd(ST7735_INVCTR);
    st7735_write_data(0x07);

    /* 显示设置 */
    st7735_write_cmd(ST7735_DISSET5);
    st7735_write_data(0x15);
    st7735_write_data(0x02);

    /* 内存数据访问控制（横屏模式） */
    st7735_write_cmd(ST7735_MADCTL);
    st7735_write_data(MADCTL_MX | MADCTL_MV | MADCTL_BGR);  /* 横屏 + BGR */

    /* 像素格式：16位 RGB565 */
    st7735_write_cmd(ST7735_COLMOD);
    st7735_write_data(ST7735_COLOR_MODE_16BIT);

    /* 关闭反转 */
    st7735_write_cmd(ST7735_INVOFF);

    /* 设置默认显示区域 */
    st7735_set_address_window(0, 0, ION_DISPLAY_WIDTH - 1, ION_DISPLAY_HEIGHT - 1);

    /* 开启显示 */
    st7735_write_cmd(ST7735_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ========== 公开 API 实现 ========== */

bool ion_display_init(void)
{
    ESP_LOGI(TAG, "Initializing ST7735 display (%dx%d)", ION_DISPLAY_WIDTH, ION_DISPLAY_HEIGHT);

    /* 配置 SPI 引脚 */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_NUM_CS) |
                        (1ULL << PIN_NUM_DC) |
                        (1ULL << PIN_NUM_RST),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    /* 初始化 CS 和 DC 为高电平 */
    gpio_set_level(PIN_NUM_CS, 1);
    gpio_set_level(PIN_NUM_DC, 1);
    gpio_set_level(PIN_NUM_RST, 1);

    /* 初始化 SPI 互斥锁 */
    ion_spi_mutex_init();

    /* 初始化 SPI 总线
     * 注意：SD 卡也使用 SPI2_HOST，此处由 LCD 先初始化总线。
     * SD 卡初始化时检测到总线已存在（ESP_ERR_INVALID_STATE）会跳过重初始化。 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,  /* 不使用 MISO */
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_SIZE * 2,  /* 最大传输大小 = 整帧 */
    };
    
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* 配置 SPI 设备 */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 40 * 1000 * 1000,  /* 40MHz */
        .mode = 0,                            /* SPI mode 0 */
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,                      /* 单队列 */
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return false;
    }

    /* 初始化序列需要 SPI 总线访问，获取互斥锁 */
    ion_spi_mutex_lock(5000);

    /* 硬件复位 */
    st7735_hardware_reset();

    /* 发送初始化序列 */
    st7735_init_sequence();

    ion_spi_mutex_unlock();

    /* 分配帧缓冲区（PSRAM 优先） */
    s_framebuffer = heap_caps_malloc(FB_SIZE * sizeof(ion_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_framebuffer == NULL) {
        s_framebuffer = malloc(FB_SIZE * sizeof(ion_color_t));
        if (s_framebuffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate framebuffer");
            return false;
        }
        ESP_LOGW(TAG, "Framebuffer allocated in DRAM");
    } else {
        ESP_LOGI(TAG, "Framebuffer allocated in PSRAM (%d KB)", (FB_SIZE * sizeof(ion_color_t)) / 1024);
    }

    /* 清空帧缓冲区 */
    memset(s_framebuffer, 0, FB_SIZE * sizeof(ion_color_t));

    /* 设置背光 */
    ion_display_set_brightness(50);

    ESP_LOGI(TAG, "Display initialized successfully");
    return true;
}

void ion_display_set_brightness(uint8_t brightness)
{
    if (brightness > BRIGHTNESS_MAX) {
        brightness = BRIGHTNESS_MAX;
    }
    s_brightness = brightness;
    
    /* 使用 PWM 控制背光（GPIO0 = LEDC 通道） */
    /* TODO: 配置 LEDC 实现 PWM 调光 */
    ESP_LOGD(TAG, "Brightness set to %d%%", brightness);
}

void ion_display_fill(ion_color_t color)
{
    if (!s_framebuffer) return;
    
    /* 填充帧缓冲区 */
    for (int i = 0; i < FB_SIZE; i++) {
        s_framebuffer[i] = color;
    }
    
    /* 刷新到屏幕 */
    ion_display_flush();
}

void ion_display_draw_pixel(int x, int y, ion_color_t color)
{
    if (!s_framebuffer) return;
    if (x < 0 || x >= ION_DISPLAY_WIDTH || y < 0 || y >= ION_DISPLAY_HEIGHT) return;
    
    s_framebuffer[y * ION_DISPLAY_WIDTH + x] = color;
}

void ion_display_draw_pixels(int x, int y, int width, int height, const ion_color_t *pixels)
{
    if (!s_framebuffer || !pixels) return;
    if (x < 0 || y < 0 || x + width > ION_DISPLAY_WIDTH || y + height > ION_DISPLAY_HEIGHT) return;
    
    /* 复制像素到帧缓冲区 */
    for (int row = 0; row < height; row++) {
        memcpy(&s_framebuffer[(y + row) * ION_DISPLAY_WIDTH + x],
               &pixels[row * width],
               width * sizeof(ion_color_t));
    }
}

ion_color_t* ion_display_get_framebuffer(void)
{
    return s_framebuffer;
}

void ion_display_flush(void)
{
    if (!s_framebuffer || !s_spi_handle) return;
    
    /* 获取 SPI 总线锁（SD 卡可能正在使用总线） */
    if (!ion_spi_mutex_lock(1000)) {
        ESP_LOGW(TAG, "SPI bus lock timeout, skipping flush");
        return;
    }
    
    /* 设置地址窗口为整个屏幕 */
    st7735_set_address_window(0, 0, ION_DISPLAY_WIDTH - 1, ION_DISPLAY_HEIGHT - 1);
    
    /* 发送 RAM 写入命令 */
    st7735_write_cmd(ST7735_RAMWR);
    
    /* 批量传输帧缓冲区数据 */
    st7735_write_data_bulk((const uint8_t *)s_framebuffer, FB_SIZE * sizeof(ion_color_t));
    
    /* 释放 SPI 总线锁 */
    ion_spi_mutex_unlock();
}