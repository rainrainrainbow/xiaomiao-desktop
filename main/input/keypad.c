#include "xiaomiao_desktop.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "keypad";

static const gpio_num_t s_btn_gpios[] = {
    PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_LEFT,
    PIN_BTN_RIGHT, PIN_BTN_A, PIN_BTN_B,
};
static const uint32_t s_btn_keys[] = {
    LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT,
    LV_KEY_RIGHT, LV_KEY_ENTER, LV_KEY_ESC,
};
#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

void keypad_init(void)
{
    uint64_t mask = 0, pullup = 0;
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        mask |= 1ULL << s_btn_gpios[i];
        /* GPIO34 and GPIO35 are input-only, no internal pull-up available */
        if (s_btn_gpios[i] != GPIO_NUM_34 && s_btn_gpios[i] != GPIO_NUM_35)
            pullup |= 1ULL << s_btn_gpios[i];
    }
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    if (pullup) {
        gpio_config_t pu = {
            .pin_bit_mask = pullup,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pu);
    }
    ESP_LOGI(TAG, "Keypad initialized (%d buttons)", NUM_BUTTONS);
}

void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int last = -1, stable = -1;
    static uint32_t changed_ms = 0;
    static uint32_t last_key = LV_KEY_ENTER;
    int raw = -1;
    uint32_t now = lv_tick_get();

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            raw = (int)i;
            break;
        }
    }

    if (raw != last) {
        last = raw;
        changed_ms = now;
        if (raw < 0) stable = -1;
    }
    if (lv_tick_elaps(changed_ms) >= BUTTON_DEBOUNCE_MS)
        stable = last;

    if (stable >= 0) {
        last_key = s_btn_keys[stable];
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = last_key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = last_key;
    }
}