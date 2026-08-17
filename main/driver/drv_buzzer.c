/**
 * @file drv_buzzer.c
 * @brief 蜂鸣器驱动 - 通过LEDC PWM产生音调
 */
#include "drv_buzzer.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DRV_BUZZER";

/* 当前音量（0-100），影响PWM占空比 */
static uint8_t s_volume = 50;

/* 获取50%占空比对应的duty值，用于音量调节 */
static uint32_t get_volume_duty(void)
{
    uint32_t max_duty = (1 << BUZZER_DUTY_RES) - 1;
    return (max_duty / 2) * s_volume / 100;
}

/* 音符编号到频率的换算：freq = 440 * 2^((note-69)/12) */
uint32_t drv_buzzer_note_to_freq(int note)
{
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    /* 使用查表方式避免浮点运算，覆盖常用音域 */
    static const uint32_t freq_table[128] = {
        8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 15,
        16, 17, 18, 19, 21, 22, 23, 25, 26, 28, 29, 31,
        33, 35, 37, 39, 41, 44, 46, 49, 52, 55, 58, 62,
        65, 69, 73, 78, 82, 87, 92, 98, 104, 110, 117, 123,
        131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247,
        262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
        523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,
        1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,
        2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951,
        4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902,
        8372, 8870, 9397, 9956, 10548, 11175, 11840, 12544
    };
    return freq_table[note];
}

void drv_buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BUZZER_TIMER,
        .duty_resolution = BUZZER_DUTY_RES,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    ESP_LOGI(TAG, "Buzzer initialized (GPIO%d)", PIN_BUZZER);
}

void drv_buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
        return;
    }
    /* 设置频率 */
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, freq_hz);
    /* 使用音量调节后的占空比 */
    uint32_t duty = get_volume_duty();
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);

    if (duration_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    }
}

void drv_buzzer_stop(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

void drv_buzzer_play_note(int note, uint32_t duration_ms)
{
    uint32_t freq = drv_buzzer_note_to_freq(note);
    drv_buzzer_tone(freq, duration_ms);
}

void drv_buzzer_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    s_volume = volume;
    ESP_LOGI(TAG, "Volume set to %d%%", volume);
}

uint8_t drv_buzzer_get_volume(void)
{
    return s_volume;
}