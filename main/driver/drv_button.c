/**
 * @file drv_button.c
 * @brief 按键驱动实现
 */

#include "drv_button.h"
#include "esp_log.h"

static const char *TAG = "DRV_BTN";

/* 按键GPIO映射表 */
static const gpio_num_t s_btn_gpios[] = {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B
};

#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

/* ========== 初始化按键驱动 ========== */
void drv_button_init(void)
{
    uint64_t mask = 0, pullup = 0;
    
    // 计算GPIO掩码
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        mask |= 1ULL << s_btn_gpios[i];
        // GPIO34/35不支持内部上拉
        if (s_btn_gpios[i] != GPIO_NUM_34 && s_btn_gpios[i] != GPIO_NUM_35) {
            pullup |= 1ULL << s_btn_gpios[i];
        }
    }
    
    // 配置所有按键为输入模式
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    
    // 为支持的GPIO启用内部上拉
    if (pullup) {
        gpio_config_t pu = {
            .pin_bit_mask = pullup,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&pu);
    }
    
    ESP_LOGI(TAG, "Button driver initialized (%d buttons)", NUM_BUTTONS);
}

/* ========== 扫描按键状态 ========== */
int drv_button_scan(void)
{
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            return (int)i;
        }
    }
    return -1;
}

/* ========== 获取按键GPIO电平 ========== */
bool drv_button_get_level(btn_idx_t btn)
{
    if (btn >= BTN_IDX_MAX) {
        return false;
    }
    return gpio_get_level(s_btn_gpios[btn]) == BUTTON_ACTIVE_LEVEL;
}