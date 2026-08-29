/**
 * @file drv_led_strip.c
 * @brief NeoPixel RGB LED灯带驱动实现 - 使用 ESP-IDF 官方 led_strip 组件 (RMT)
 *
 * 硬件：
 * - NeoPixel RGB (NEO_RGB 位序 R->G->B), 3 颗, 连接 GPIO14 (与蜂鸣器复用)
 * - 通过官方 espressif/led_strip 组件驱动，内部封装 RMT 时序编码与复位信号
 *
 * 与蜂鸣器互斥：
 * - GPIO14 同时连接蜂鸣器（LEDC PWM）和 NeoPixel RGB（RMT）
 * - 使用前需停止蜂鸣器，使用后释放 GPIO
 */
#include "drv_led_strip.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

/* 每个LED需要 24 个 RMT symbols（8bit x 3色），3颗=72个。
 * 官方示例的 64 只够 1 颗，多个 LED 必须扩容，否则 RMT 传输被拒绝/截断 */
#define LED_STRIP_RMT_MEM_SYMBOLS  128

static const char *TAG = "DRV_LED";

/* ========== 引脚定义 ========== */
#define PIN_LED_STRIP   GPIO_NUM_14     /* NeoPixel RGB 数据引脚（与蜂鸣器复用） */

/* ========== 内部状态 ========== */
static led_strip_handle_t s_strip = NULL;   /* 官方 led_strip 组件句柄 */
static uint8_t s_brightness = 128;           /* 默认50%亮度 */
static bool s_initialized = false;
static volatile bool s_breathing = false;

/* ========== 公共接口 ========== */

esp_err_t drv_led_strip_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing NeoPixel RGB LED strip (official led_strip) on GPIO%d", PIN_LED_STRIP);

    /* 强制释放 GPIO14 上的外设占用（蜂鸣器已拆除，但启动时 drv_buzzer_init()
     * 可能仍将 LEDC 绑定到 GPIO14；RMT 分配前必须重置引脚，否则 RMT 信号发不出去） */
    gpio_reset_pin(PIN_LED_STRIP);
    gpio_set_direction(PIN_LED_STRIP, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED_STRIP, 0);

    /* 官方 led_strip 组件配置：RMT 后端，RGB 位序（匹配 NEO_RGB） */
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_STRIP,
        .max_leds = LED_STRIP_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  /* R->G->B，NEO_RGB */
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10MHz，0.1us 精度 */
        .mem_block_symbols = LED_STRIP_RMT_MEM_SYMBOLS,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        s_strip = NULL;
        return ret;
    }

    /* 初始清空灯带 */
    if (s_strip) {
        led_strip_clear(s_strip);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NeoPixel RGB LED strip initialized (official led_strip, %d LEDs)", LED_STRIP_COUNT);
    return ESP_OK;
}

esp_err_t drv_led_strip_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    /* 先清空灯带 */
    drv_led_strip_clear();
    /* 释放 LED strip 组件（内部会释放 RMT 通道资源） */
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_initialized = false;
    ESP_LOGI(TAG, "NeoPixel RGB LED strip deinitialized");
    return ESP_OK;
}

esp_err_t drv_led_strip_set_pixel(int index, led_rgb_t color)
{
    if (index < 0 || index >= LED_STRIP_COUNT || !s_strip) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 应用全局亮度缩放 */
    uint8_t r = (uint8_t)((uint32_t)color.r * s_brightness / 255);
    uint8_t g = (uint8_t)((uint32_t)color.g * s_brightness / 255);
    uint8_t b = (uint8_t)((uint32_t)color.b * s_brightness / 255);
    return led_strip_set_pixel(s_strip, (uint32_t)index, r, g, b);
}

esp_err_t drv_led_strip_set_all(led_rgb_t color)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        esp_err_t ret = drv_led_strip_set_pixel(i, color);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t drv_led_strip_refresh(void)
{
    if (!s_initialized || !s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 官方组件内部处理 RMT 传输与复位时序 */
    return led_strip_refresh(s_strip);
}

esp_err_t drv_led_strip_clear(void)
{
    if (!s_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_strip);
}

void drv_led_strip_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

uint8_t drv_led_strip_get_brightness(void)
{
    return s_brightness;
}

void drv_led_strip_breath(led_rgb_t color, uint32_t duration_ms)
{
    if (!s_initialized) return;

    s_breathing = true;
    uint32_t elapsed = 0;
    uint32_t step_ms = 30;  /* 每30ms更新一次 */

    while (s_breathing) {
        for (int phase = 0; phase < 256 && s_breathing; phase += 4) {
            float rad = (float)phase * 3.14159f / 128.0f;
            uint8_t breath_brightness = (uint8_t)(128 + 127 * sinf(rad));

            led_rgb_t scaled = {
                .r = (uint8_t)((uint32_t)color.r * breath_brightness / 255),
                .g = (uint8_t)((uint32_t)color.g * breath_brightness / 255),
                .b = (uint8_t)((uint32_t)color.b * breath_brightness / 255),
            };
            drv_led_strip_set_all(scaled);
            drv_led_strip_refresh();

            if (duration_ms > 0) {
                elapsed += step_ms;
                if (elapsed >= duration_ms) {
                    s_breathing = false;
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }

    drv_led_strip_clear();
}

void drv_led_strip_breath_stop(void)
{
    s_breathing = false;
}

bool drv_led_strip_is_active(void)
{
    return s_initialized;
}
