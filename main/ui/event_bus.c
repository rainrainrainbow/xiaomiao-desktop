/**
 * @file event_bus.c
 * @brief 事件总线系统实现（借鉴 X-TRACK DataCenter）
 */

#include "event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "EVENT_BUS";

/* ========== 全局订阅者表 ========== */
static subscriber_t s_subscribers[EVENT_MAX][MAX_SUBSCRIBERS_PER_EVENT];

/* ========== 线程安全保护 ========== */
static SemaphoreHandle_t s_bus_mutex = NULL;

/* ========== 初始化 ========== */
void event_bus_init(void)
{
    memset(s_subscribers, 0, sizeof(s_subscribers));
    s_bus_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Event bus initialized");
}

/* ========== 订阅事件 ========== */
bool event_subscribe(event_type_t event_type, event_callback_t callback, void *user_data)
{
    if (event_type >= EVENT_MAX || !callback) {
        ESP_LOGE(TAG, "Invalid event type or callback");
        return false;
    }
    
    if (s_bus_mutex) xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    
    /* 查找空闲槽位 */
    for (int i = 0; i < MAX_SUBSCRIBERS_PER_EVENT; i++) {
        if (!s_subscribers[event_type][i].active) {
            s_subscribers[event_type][i].callback = callback;
            s_subscribers[event_type][i].user_data = user_data;
            s_subscribers[event_type][i].active = true;
            ESP_LOGI(TAG, "Subscribed to event %d (slot %d)", event_type, i);
            if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
            return true;
        }
    }
    
    if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
    ESP_LOGW(TAG, "No free slot for event %d", event_type);
    return false;
}

/* ========== 取消订阅 ========== */
bool event_unsubscribe(event_type_t event_type, event_callback_t callback)
{
    if (event_type >= EVENT_MAX || !callback) {
        return false;
    }
    
    if (s_bus_mutex) xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    
    for (int i = 0; i < MAX_SUBSCRIBERS_PER_EVENT; i++) {
        if (s_subscribers[event_type][i].active && 
            s_subscribers[event_type][i].callback == callback) {
            s_subscribers[event_type][i].active = false;
            s_subscribers[event_type][i].user_data = NULL;
            ESP_LOGI(TAG, "Unsubscribed from event %d (slot %d)", event_type, i);
            if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
            return true;
        }
    }
    
    if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
    return false;
}

/* ========== 发布事件 ========== */
void event_publish(event_type_t event_type, const void *data, uint32_t size)
{
    if (event_type >= EVENT_MAX) {
        ESP_LOGE(TAG, "Invalid event type: %d", event_type);
        return;
    }
    
    event_data_t event = {
        .type = event_type,
        .data = (void *)data,
        .size = size,
    };
    
    /* 发布时加锁，防止订阅/取消订阅并发修改 */
    if (s_bus_mutex) xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    
    int notified = 0;
    for (int i = 0; i < MAX_SUBSCRIBERS_PER_EVENT; i++) {
        if (s_subscribers[event_type][i].active) {
            s_subscribers[event_type][i].callback(&event, s_subscribers[event_type][i].user_data);
            notified++;
        }
    }
    
    if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
    
    if (notified > 0) {
        ESP_LOGD(TAG, "Event %d published to %d subscribers", event_type, notified);
    }
}

/* ========== 清空订阅者 ========== */
void event_clear_subscribers(event_type_t event_type)
{
    if (event_type >= EVENT_MAX) {
        return;
    }
    
    if (s_bus_mutex) xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    memset(s_subscribers[event_type], 0, sizeof(s_subscribers[event_type]));
    if (s_bus_mutex) xSemaphoreGive(s_bus_mutex);
    
    ESP_LOGI(TAG, "Cleared all subscribers for event %d", event_type);
}