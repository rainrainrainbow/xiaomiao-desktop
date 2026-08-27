/**
 * @file drv_led_strip.c
 * @brief WS2812B LED灯带驱动实现 - 使用RMT，与蜂鸣器复用GPIO14
 *
 * WS2812B 时序（使用 RMT）：
 * - T0H = 0.35us, T0L = 0.80us (逻辑0)
 * - T1H = 0.70us, T1L = 0.60us (逻辑1)
 * - RESET = >50us 低电平
 *
 * 与蜂鸣器互斥：
 * - GPIO14 同时连接蜂鸣器（LEDC PWM）和 WS2812B（RMT）
 * - 使用前需停止蜂鸣器，使用后恢复
 */
#include "drv_led_strip.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "DRV_LED";

/* ========== 引脚定义 ========== */
#define PIN_LED_STRIP   GPIO_NUM_14     /* WS2812B 数据引脚（与蜂鸣器复用） */

/* ========== WS2812B 时序参数 ========== */
#define RMT_LED_RESOLUTION_HZ   10000000  /* 10MHz = 0.1us 精度 */
#define RMT_T0H                 35        /* 0.35us */
#define RMT_T0L                 80        /* 0.80us */
#define RMT_T1H                 70        /* 0.70us */
#define RMT_T1L                 60        /* 0.60us */
#define RMT_RESET_US            80        /* 80us 复位信号 */

/* ========== 内部状态 ========== */
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static led_rgb_t s_led_buffer[LED_STRIP_COUNT];
static uint8_t s_brightness = 128;  /* 默认50%亮度 */
static bool s_initialized = false;
static volatile bool s_breathing = false;

/* ========== RMT 编码器 ========== */
/* 使用 ESP-IDF 内置的 bytes_encoder */
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    int state;
    size_t cur_pixel;   /* 当前处理的像素索引 */
    size_t cur_byte;    /* 当前处理的字节索引 (0=G, 1=R, 2=B) */
} led_encoder_t;

static size_t rmt_encode_led(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                             const void *primary_data, size_t data_size,
                             rmt_encode_state_t *ret_state)
{
    led_encoder_t *led_enc = __containerof(encoder, led_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_size = 0;

    const led_rgb_t *pixels = (const led_rgb_t *)primary_data;
    int num_pixels = data_size / sizeof(led_rgb_t);

    if (num_pixels <= 0) {
        *ret_state = RMT_ENCODING_COMPLETE;
        return 0;
    }

    switch (led_enc->state) {
    case 0: /* 发送像素数据 (GRB 顺序) */
        while (led_enc->cur_pixel < (size_t)num_pixels) {
            /* 计算带亮度调整的 GRB 值 */
            uint8_t grb[3] = {
                (uint8_t)(pixels[led_enc->cur_pixel].g * s_brightness / 255),
                (uint8_t)(pixels[led_enc->cur_pixel].r * s_brightness / 255),
                (uint8_t)(pixels[led_enc->cur_pixel].b * s_brightness / 255),
            };

            /* 编码当前字节 */
            size_t size = led_enc->bytes_encoder->encode(
                led_enc->bytes_encoder, channel, &grb[led_enc->cur_byte], 1, &session_state);
            encoded_size += size;

            /* 移动到下一个字节/像素 */
            led_enc->cur_byte++;
            if (led_enc->cur_byte >= 3) {
                led_enc->cur_byte = 0;
                led_enc->cur_pixel++;
            }

            /* 如果本次编码未完成，返回让 RMT 继续请求数据 */
            if (!(session_state & RMT_ENCODING_COMPLETE)) {
                *ret_state = RMT_ENCODING_RESET;
                return encoded_size;
            }
        }
        /* 所有像素发送完毕，进入复位阶段 */
        led_enc->state = 1;
        led_enc->cur_pixel = 0;
        led_enc->cur_byte = 0;
        /* fall through */

    case 1: /* 发送复位信号 (>50us 低电平) */
        /* WS2812B 复位信号：至少 50us 的低电平 */
        /* 这里不需要额外编码，RMT 在传输完成后会自动保持 eot_level=0 */
        led_enc->state = 0;
        *ret_state = RMT_ENCODING_COMPLETE;
        break;

    default:
        *ret_state = RMT_ENCODING_RESET;
        break;
    }

    return encoded_size;
}

static esp_err_t rmt_del_led_encoder(rmt_encoder_t *encoder)
{
    led_encoder_t *led_enc = __containerof(encoder, led_encoder_t, base);
    if (led_enc->bytes_encoder) {
        rmt_del_encoder(led_enc->bytes_encoder);
    }
    free(led_enc);
    return ESP_OK;
}

static esp_err_t rmt_led_encoder_reset(rmt_encoder_t *encoder)
{
    led_encoder_t *led_enc = __containerof(encoder, led_encoder_t, base);
    if (led_enc->bytes_encoder) {
        rmt_encoder_reset(led_enc->bytes_encoder);
    }
    led_enc->state = 0;
    led_enc->cur_pixel = 0;
    led_enc->cur_byte = 0;
    return ESP_OK;
}

/* ========== 公共接口 ========== */

esp_err_t drv_led_strip_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WS2812B LED strip on GPIO%d", PIN_LED_STRIP);

    /* ---- 配置 RMT TX 通道 ---- */
    rmt_tx_channel_config_t tx_chan_cfg = {
        .gpio_num = PIN_LED_STRIP,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_LED_RESOLUTION_HZ,
        .mem_block_symbols = 128,
        .trans_queue_depth = 4,
        .flags = {
            .invert_out = false,
            .with_dma = false,
            .io_loop_back = false,
            .io_od_mode = false,
        },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_cfg, &s_led_chan));

    /* ---- 创建编码器 ---- */
    led_encoder_t *enc = heap_caps_calloc(1, sizeof(led_encoder_t), MALLOC_CAP_8BIT);
    if (!enc) {
        ESP_LOGE(TAG, "Failed to allocate encoder");
        return ESP_ERR_NO_MEM;
    }
    enc->base.encode = rmt_encode_led;
    enc->base.del = rmt_del_led_encoder;
    enc->base.reset = rmt_led_encoder_reset;
    s_led_encoder = &enc->base;

    /* 创建 bytes_encoder 用于 WS2812B 位编码 */
    rmt_bytes_encoder_config_t bytes_enc_cfg = {
        .bit0 = {
            .duration0 = RMT_T0H,
            .level0 = 1,
            .duration1 = RMT_T0L,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = RMT_T1H,
            .level0 = 1,
            .duration1 = RMT_T1L,
            .level1 = 0,
        },
        .flags.msb_first = true,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_enc_cfg, &enc->bytes_encoder));

    /* ---- 启用 RMT TX 通道 ---- */
    ESP_ERROR_CHECK(rmt_enable(s_led_chan));

    /* 清空缓冲区 */
    memset(s_led_buffer, 0, sizeof(s_led_buffer));

    s_initialized = true;
    ESP_LOGI(TAG, "WS2812B LED strip initialized (%d LEDs)", LED_STRIP_COUNT);
    return ESP_OK;
}

esp_err_t drv_led_strip_set_pixel(int index, led_rgb_t color)
{
    if (index < 0 || index >= LED_STRIP_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    s_led_buffer[index] = color;
    return ESP_OK;
}

esp_err_t drv_led_strip_set_all(led_rgb_t color)
{
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        s_led_buffer[i] = color;
    }
    return ESP_OK;
}

esp_err_t drv_led_strip_refresh(void)
{
    if (!s_initialized || !s_led_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 发送数据到 LED 灯带 */
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,  /* 发送完后拉低 */
        },
    };

    esp_err_t ret = rmt_transmit(s_led_chan, s_led_encoder, s_led_buffer,
                                  sizeof(s_led_buffer), &tx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 等待发送完成 */
    ret = rmt_tx_wait_all_done(s_led_chan, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RMT wait timeout");
    }

    return ESP_OK;
}

esp_err_t drv_led_strip_clear(void)
{
    memset(s_led_buffer, 0, sizeof(s_led_buffer));
    return drv_led_strip_refresh();
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
        /* 正弦波呼吸效果 */
        for (int phase = 0; phase < 256 && s_breathing; phase += 4) {
            /* brightness = 128 + 127 * sin(phase * 2pi / 256) */
            float rad = (float)phase * 3.14159f / 128.0f;
            uint8_t breath_brightness = (uint8_t)(128 + 127 * sinf(rad));

            led_rgb_t scaled = {
                .r = color.r * breath_brightness / 255,
                .g = color.g * breath_brightness / 255,
                .b = color.b * breath_brightness / 255,
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

    /* 关闭所有 LED */
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