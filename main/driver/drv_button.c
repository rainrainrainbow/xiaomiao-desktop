/**
 * @file drv_button.c
 * @brief 按键驱动实现 - 独立任务 + 事件队列
 */

#include "drv_button.h"
#include "drv_battery.h"   // 复用电池ADC读取A键(GPIO34)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "driver/adc_types_legacy.h"  // for adc_channel_t

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
    int64_t press_time;     // 当前按键按下时间（连续稳定被按下）
    bool long_sent;         // 长按事件是否已发送
} btn_state_t;

static btn_state_t s_btn_state = {
    .last_raw = -1,
    .stable = -1,
    .change_time = 0,
    .debounce_done = true,
    .press_time = 0,
    .long_sent = false
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

    // 创建事件队列（存储 btn_event_t）
    if (s_btn_queue == NULL) {
        s_btn_queue = xQueueCreate(16, sizeof(btn_event_t));
    }

    ESP_LOGI(TAG, "Button driver initialized (%d buttons, debounce=%dms)",
             NUM_BUTTONS, BUTTON_DEBOUNCE_MS);
}

/* ========== 扫描按键状态（原始） ========== */
static int drv_button_scan_raw(void)
{
    // 先扫描非ADC共享的可靠引脚 (UP, DOWN, LEFT, RIGHT, B)
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        if (i == BTN_IDX_A) continue;  // 跳过A键(GPIO34, ADC共享)
        if (gpio_get_level(s_btn_gpios[i]) == BUTTON_ACTIVE_LEVEL) {
            return (int)i;
        }
    }
    
    // A 键(GPIO34/ADC1_CH6)：通过ADC读取电压判断
    // 按下时接地≈0V (raw≈0)，松开时外部上拉≈高电平 (raw接近满量程)
    int raw_adc = 0;
    esp_err_t ret = drv_battery_read_raw(ADC_CHANNEL_6, &raw_adc);
    if (ret != ESP_OK) {
        return -1;  // ADC读取失败，认为A键未按下
    }
    
    // 阈值判断：12-bit ADC满量程4095，按下时接近0
    // 设定阈值为 500（约0.4V），低于此值认为按下
    #define A_BTN_ADC_THRESHOLD  500
    if (raw_adc < A_BTN_ADC_THRESHOLD) {
        return BTN_IDX_A;
    }
    
    return -1;  // 无按键按下
}

/* ========== 按键任务 ========== */
void drv_button_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Button task started");
    
    while (1) {
        int raw = drv_button_scan_raw();
        int64_t now = esp_timer_get_time();
        
        // 状态变化检测
        if (raw != s_btn_state.last_raw) {
            s_btn_state.last_raw = raw;
            s_btn_state.change_time = now;
            s_btn_state.debounce_done = false;
            // 按键变化时如果之前有按下的键，说明按键状态改变了
        }
        
        // 去抖完成检测
        if (!s_btn_state.debounce_done && 
            (now - s_btn_state.change_time) >= (int64_t)BUTTON_DEBOUNCE_MS * 1000) {
            s_btn_state.debounce_done = true;
            
            // 状态从"有按键"切换到"新按键"或"无按键"
            if (raw != s_btn_state.stable) {
                // 如果之前有按键稳定按下，且现在变为无按键 → 短按事件（释放）
                if (s_btn_state.stable >= 0 && raw < 0) {
                    // 仅在未触发过长按时发送短按事件（长按后释放不再重复）
                    if (!s_btn_state.long_sent) {
                        btn_event_t evt = { .key = s_btn_state.stable, .is_long_press = false };
                        if (s_btn_queue != NULL) {
                            xQueueSend(s_btn_queue, &evt, 0);
                            ESP_LOGI(TAG, "Button short-press: %d", evt.key);
                        }
                    }
                }
                
                // 更新稳定状态
                s_btn_state.stable = raw;
                s_btn_state.long_sent = false;
                
                // 记录按下起始时间
                if (raw >= 0) {
                    s_btn_state.press_time = now;
                }
            }
        }
        
        // 长按检测：持续按住同一按键超过 LONG_PRESS_MS
        if (s_btn_state.stable >= 0 && !s_btn_state.long_sent &&
            (now - s_btn_state.press_time) >= (int64_t)LONG_PRESS_MS * 1000) {
            s_btn_state.long_sent = true;
            btn_event_t evt = { .key = s_btn_state.stable, .is_long_press = true };
            if (s_btn_queue != NULL) {
                xQueueSend(s_btn_queue, &evt, 0);
                ESP_LOGI(TAG, "Button LONG-press: %d", evt.key);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(5));  // 5ms 扫描周期
    }
}

/* ========== 获取按键事件（非阻塞） ========== */
bool drv_button_get_event(btn_event_t *evt)
{
    if (s_btn_queue != NULL && evt != NULL) {
        return xQueueReceive(s_btn_queue, evt, 0) == pdTRUE;
    }
    return false;
}

/* ========== 获取当前按下的按键（阻塞版本） ========== */
bool drv_button_wait_press(uint32_t timeout_ms, btn_event_t *evt)
{
    if (s_btn_queue != NULL && evt != NULL) {
        return xQueueReceive(s_btn_queue, evt, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
    return false;
}