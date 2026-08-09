/**
 * @file drv_button.c
 * @brief 按键驱动实现 - 独立任务 + 事件队列
 */

#include "drv_button.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

static const char *TAG = "DRV_BTN";

/* 按键GPIO映射表 */
static const gpio_num_t s_btn_gpios[] = {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B
};

#define NUM_BUTTONS (sizeof(s_btn_gpios) / sizeof(s_btn_gpios[0]))

/* 事件队列 */
static QueueHandle_t s_btn_queue = NULL;

/* 按键状态 */
typedef struct {
    int last_raw;           // 上次原始值
    int stable;             // 稳定后的值
    int64_t change_time;    // 状态变化时间
    bool debounce_done;     // 去抖完成标志
} btn_state_t;

static btn_state_t s_btn_state = {
    .last_raw = -1,
    .stable = -1,
    .change_time = 0,
    .debounce_done = true
};

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

    // 创建事件队列
    if (s_btn_queue == NULL) {
        s_btn_queue = xQueueCreate(10, sizeof(int));
    }

    ESP_LOGI(TAG, "Button driver initialized (%d buttons, debounce=%dms)",
             NUM_BUTTONS, BUTTON_DEBOUNCE_MS);
}

/* ========== 扫描按键状态（原始） ========== */
static int drv_button_scan_raw(void)
{
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            return (int)i;
        }
    }
    return -1;
}

/* ========== 按键任务 ========== */
void drv_button_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Button task started");
    
    int last_event = -1;  // 上次触发的事件
    
    while (1) {
        int raw = drv_button_scan_raw();
        int64_t now = esp_timer_get_time();
        
        // 状态变化检测
        if (raw != s_btn_state.last_raw) {
            s_btn_state.last_raw = raw;
            s_btn_state.change_time = now;
            s_btn_state.debounce_done = false;
        }
        
        // 去抖完成检测
        if (!s_btn_state.debounce_done && 
            (now - s_btn_state.change_time) >= (int64_t)BUTTON_DEBOUNCE_MS * 1000) {
            s_btn_state.stable = raw;
            s_btn_state.debounce_done = true;
            
            // 检测到按键按下（边沿触发）
            if (raw >= 0 && raw != last_event) {
                // 发送事件到队列
                if (s_btn_queue != NULL) {
                    xQueueSend(s_btn_queue, &raw, 0);
                    ESP_LOGD(TAG, "Button event: %d", raw);
                }
                last_event = raw;
            } else if (raw < 0) {
                // 按键释放，重置事件状态
                last_event = -1;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(5));  // 5ms 扫描周期
    }
}

/* ========== 获取按键事件（非阻塞） ========== */
int drv_button_get_event(void)
{
    int event = -1;
    if (s_btn_queue != NULL) {
        xQueueReceive(s_btn_queue, &event, 0);
    }
    return event;
}

/* ========== 获取当前按下的按键（阻塞版本） ========== */
int drv_button_wait_press(uint32_t timeout_ms)
{
    int event = -1;
    if (s_btn_queue != NULL) {
        xQueueReceive(s_btn_queue, &event, pdMS_TO_TICKS(timeout_ms));
    }
    return event;
}