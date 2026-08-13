/**
 * @file event_bus.h
 * @brief 事件总线系统（借鉴 X-TRACK DataCenter 消息订阅发布框架）
 * 
 * 设计思想：
 * - 按键驱动作为 Publisher，发布按键事件
 * - 页面/模块作为 Subscriber，订阅感兴趣的事件
 * - 解耦按键处理逻辑，提升可扩展性
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>
#include <stdbool.h>

/* ========== 事件类型 ========== */
typedef enum {
    EVENT_KEY_PRESS,      // 按键按下（短按）
    EVENT_KEY_LONG_PRESS, // 按键长按
    EVENT_KEY_RELEASE,    // 按键释放
    EVENT_BATTERY_UPDATE, // 电池更新
    EVENT_TIME_UPDATE,    // 时间更新
    EVENT_MAX
} event_type_t;

/* ========== 按键事件数据 ========== */
typedef struct {
    int key;              // 按键索引（UP/DOWN/LEFT/RIGHT/A/B）
    bool is_long_press;   // 是否长按
    uint32_t timestamp;   // 事件时间戳（lv_tick）
} key_event_data_t;

/* ========== 通用事件数据 ========== */
typedef struct {
    event_type_t type;    // 事件类型
    void *data;           // 事件数据指针
    uint32_t size;        // 数据大小
} event_data_t;

/* ========== 事件回调函数 ========== */
typedef void (*event_callback_t)(const event_data_t *event, void *user_data);

/* ========== 订阅者结构 ========== */
#define MAX_SUBSCRIBERS_PER_EVENT 8

typedef struct {
    event_callback_t callback;
    void *user_data;
    bool active;
} subscriber_t;

/* ========== 事件总线 API ========== */

/**
 * 初始化事件总线
 */
void event_bus_init(void);

/**
 * 订阅事件
 * @param event_type 事件类型
 * @param callback 回调函数
 * @param user_data 用户数据（传递给回调）
 * @return true 成功，false 失败（订阅者已满）
 */
bool event_subscribe(event_type_t event_type, event_callback_t callback, void *user_data);

/**
 * 取消订阅
 * @param event_type 事件类型
 * @param callback 回调函数
 * @return true 成功，false 未找到
 */
bool event_unsubscribe(event_type_t event_type, event_callback_t callback);

/**
 * 发布事件
 * @param event_type 事件类型
 * @param data 事件数据
 * @param size 数据大小
 */
void event_publish(event_type_t event_type, const void *data, uint32_t size);

/**
 * 清空某事件的所有订阅者
 * @param event_type 事件类型
 */
void event_clear_subscribers(event_type_t event_type);

#endif /* EVENT_BUS_H */
