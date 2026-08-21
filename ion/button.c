/**
 * @file ion/button.c
 * @brief Ion - Hardware Abstraction Layer: Button Implementation
 * 
 * 6键手柄驱动实现（UP, DOWN, LEFT, RIGHT, A, B）。
 * A 键使用 ADC 读取（GPIO34 与电池 ADC 共享），
 * 其他按键使用 GPIO 读取。
 */

#include "ion/button.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ION_BUTTON";

/* ========== 硬件引脚定义 ========== */
#define PIN_BUTTON_UP    GPIO_NUM_2
#define PIN_BUTTON_DOWN  GPIO_NUM_13
#define PIN_BUTTON_LEFT  GPIO_NUM_27
#define PIN_BUTTON_RIGHT GPIO_NUM_35
#define PIN_BUTTON_B     GPIO_NUM_12
#define PIN_BUTTON_A     GPIO_NUM_34  /* ADC 读取 */

/* ========== ADC 配置 ========== */
#define ADC_CHANNEL_A    ADC_CHANNEL_6  /* GPIO34 = ADC1_CH6 */
#define ADC_THRESHOLD    500            /* 按键按下阈值（电压 < 0.4V） */

/* ========== 按键状态 ========== */
#define DEBOUNCE_MS      20             /* 去抖时间（毫秒） */
#define LONG_PRESS_MS    1000           /* 长按时间（毫秒） */

/* ========== 按键状态机 ========== */
typedef enum {
    BUTTON_STATE_RELEASED = 0,
    BUTTON_STATE_DEBOUNCE,    /* 去抖中 */
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_LONG_PRESS
} button_fsm_state_t;

typedef struct {
    button_fsm_state_t fsm;     /* 状态机状态 */
    ion_button_state_t state;   /* 对外状态 */
    uint32_t press_start_ms;    /* 按下开始时间 */
    uint32_t last_scan_ms;      /* 上次扫描时间 */
} button_state_t;

static button_state_t s_buttons[ION_BUTTON_COUNT];
static ion_button_t s_last_pressed = ION_BUTTON_COUNT;

/* ========== ADC 读取 A 键 ========== */
static bool read_button_a(void)
{
    int raw;
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL_A, ADC_ATTEN_DB_11);
    raw = adc1_get_raw(ADC_CHANNEL_A);
    return (raw < ADC_THRESHOLD);
}

/* ========== GPIO 读取其他按键 ========== */
static bool read_button_gpio(ion_button_t button)
{
    gpio_num_t pin;
    switch (button) {
        case ION_BUTTON_UP:    pin = PIN_BUTTON_UP; break;
        case ION_BUTTON_DOWN:  pin = PIN_BUTTON_DOWN; break;
        case ION_BUTTON_LEFT:  pin = PIN_BUTTON_LEFT; break;
        case ION_BUTTON_RIGHT: pin = PIN_BUTTON_RIGHT; break;
        case ION_BUTTON_B:     pin = PIN_BUTTON_B; break;
        default: return false;
    }
    return (gpio_get_level(pin) == 0);  /* 低电平有效 */
}

/* ========== 公开 API 实现 ========== */

bool ion_button_init(void)
{
    ESP_LOGI(TAG, "Initializing buttons");

    /* 配置 GPIO 按键引脚（上拉输入） */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUTTON_UP) |
                        (1ULL << PIN_BUTTON_DOWN) |
                        (1ULL << PIN_BUTTON_LEFT) |
                        (1ULL << PIN_BUTTON_RIGHT) |
                        (1ULL << PIN_BUTTON_B),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    /* 初始化 ADC（用于 A 键） */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL_A, ADC_ATTEN_DB_11);

    /* 初始化状态 */
    for (int i = 0; i < ION_BUTTON_COUNT; i++) {
        s_buttons[i].fsm = BUTTON_STATE_RELEASED;
        s_buttons[i].state = ION_BUTTON_STATE_RELEASED;
        s_buttons[i].press_start_ms = 0;
        s_buttons[i].last_scan_ms = 0;
    }

    ESP_LOGI(TAG, "Buttons initialized");
    return true;
}

void ion_button_scan(void)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    for (int i = 0; i < ION_BUTTON_COUNT; i++) {
        bool pressed = (i == ION_BUTTON_A) ? read_button_a() : read_button_gpio((ion_button_t)i);
        button_state_t *btn = &s_buttons[i];

        switch (btn->fsm) {
            case BUTTON_STATE_RELEASED:
                if (pressed) {
                    /* 进入去抖状态 */
                    btn->fsm = BUTTON_STATE_DEBOUNCE;
                    btn->last_scan_ms = now;
                    btn->state = ION_BUTTON_STATE_RELEASED;
                }
                break;

            case BUTTON_STATE_DEBOUNCE:
                if (pressed) {
                    /* 持续按下时间达到去抖阈值，确认按下 */
                    if (now - btn->last_scan_ms >= DEBOUNCE_MS) {
                        btn->fsm = BUTTON_STATE_PRESSED;
                        btn->state = ION_BUTTON_STATE_PRESSED;
                        btn->press_start_ms = now;
                        s_last_pressed = (ion_button_t)i;
                    }
                } else {
                    /* 去抖期间释放，回到释放状态 */
                    btn->fsm = BUTTON_STATE_RELEASED;
                    btn->state = ION_BUTTON_STATE_RELEASED;
                }
                break;

            case BUTTON_STATE_PRESSED:
                if (!pressed) {
                    /* 释放按键 */
                    btn->fsm = BUTTON_STATE_RELEASED;
                    btn->state = ION_BUTTON_STATE_RELEASED;
                } else if (now - btn->press_start_ms >= LONG_PRESS_MS) {
                    /* 达到长按时间阈值 */
                    btn->fsm = BUTTON_STATE_LONG_PRESS;
                    btn->state = ION_BUTTON_STATE_LONG_PRESS;
                }
                break;

            case BUTTON_STATE_LONG_PRESS:
                if (!pressed) {
                    /* 长按后释放 */
                    btn->fsm = BUTTON_STATE_RELEASED;
                    btn->state = ION_BUTTON_STATE_RELEASED;
                }
                break;
        }
    }
}

ion_button_state_t ion_button_get_state(ion_button_t button)
{
    if (button >= ION_BUTTON_COUNT) return ION_BUTTON_STATE_RELEASED;
    return s_buttons[button].state;
}

bool ion_button_any_pressed(void)
{
    for (int i = 0; i < ION_BUTTON_COUNT; i++) {
        if (s_buttons[i].state != ION_BUTTON_STATE_RELEASED) {
            return true;
        }
    }
    return false;
}

ion_button_t ion_button_last_pressed(void)
{
    return s_last_pressed;
}

ion_button_t ion_button_wait(uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    while (1) {
        ion_button_scan();
        
        for (int i = 0; i < ION_BUTTON_COUNT; i++) {
            if (s_buttons[i].state == ION_BUTTON_STATE_PRESSED) {
                return (ion_button_t)i;
            }
        }
        
        if (timeout_ms > 0) {
            uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;
            if (elapsed >= timeout_ms) {
                return ION_BUTTON_COUNT;  /* 超时 */
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}