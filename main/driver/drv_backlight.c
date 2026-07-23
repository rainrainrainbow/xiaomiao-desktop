/**
 * @file drv_backlight.c
 * @brief 背光驱动实现
 */

#include "drv_backlight.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "DRV_BL";
static int s_current_brightness = 75;

/* ========== 初始化背光PWM ========== */
void drv_backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 初始化时设置为最亮（duty=8191，高电平点亮）
    uint32_t initial_duty = 8191;  // 100%亮度
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_LCD_BL,
        .duty = initial_duty,  // 最亮
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    
    s_current_brightness = 100;
    
    ESP_LOGW(TAG, "Backlight PWM initialized (GPIO%d, %dHz, 13-bit, duty=%lu=MAX)", 
             PIN_LCD_BL, LEDC_FREQ_HZ, initial_duty);
}

/* ========== 设置背光亮度 ========== */
void drv_backlight_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    s_current_brightness = percent;
    
    // 13-bit resolution: 0-8191
    // 硬件是高电平点亮，duty越大越亮
    uint32_t duty = (8191 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
    
    ESP_LOGI(TAG, "Backlight set to %d%% (duty=%lu)", percent, duty);
}

/* ========== 获取当前亮度 ========== */
int drv_backlight_get_brightness(void)
{
    return s_current_brightness;
}