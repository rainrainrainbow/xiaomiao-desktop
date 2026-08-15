/**
 * @file ion/battery.h
 * @brief Ion - Hardware Abstraction Layer: Battery Interface
 * 
 * 参考 NumWorks Epsilon 的 Ion 层设计，提供统一的电池管理接口。
 */

#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化电池检测驱动
 * @return true 成功，false 失败
 */
bool ion_battery_init(void);

/**
 * @brief 读取电池电压（毫伏）
 * @return 电池电压（mV），0 表示读取失败
 */
uint16_t ion_battery_read_voltage_mv(void);

/**
 * @brief 获取电池电量百分比
 * @return 电量百分比（0-100）
 */
uint8_t ion_battery_get_percentage(void);

/**
 * @brief 检查电池是否正在充电
 * @return true 充电中，false 未充电
 */
bool ion_battery_is_charging(void);

/**
 * @brief 获取电池状态字符串
 * @return 状态字符串（如 "85%", "充电中", "低电量"）
 */
const char* ion_battery_get_status_string(void);

#endif /* BATTERY_H */