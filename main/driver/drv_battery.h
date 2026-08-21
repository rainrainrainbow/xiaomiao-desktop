/**
 * @file drv_battery.h
 * @brief 电池驱动 - ADC采样和电量计算
 */

#ifndef DRV_BATTERY_H
#define DRV_BATTERY_H

#include "esp_err.h"
#include "hal/adc_types.h"

/* ========== 电池ADC定义 ========== */
#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_6
#define BAT_VDIV_R1         3300    // 上拉电阻 3.3kΩ（经校准：Vbat = Vadc * 1.33）
#define BAT_VDIV_R2         10000   // 下拉电阻 10kΩ
#define BAT_DIODE_DROP      0.30f   // 防反接二极管压降（K23典型值0.3V）
#define BAT_ADC_ATTEN       ADC_ATTEN_DB_12
#define BAT_ADC_BITWIDTH    ADC_BITWIDTH_12
#define BAT_MIN_VALID_V     2.5f

/* ========== 电池驱动接口 ========== */

/**
 * 初始化电池ADC
 */
void drv_battery_init(void);

/**
 * 读取电池电压
 * @return 电池电压（伏特）
 */
float drv_battery_get_voltage(void);

/**
 * 计算电池电量百分比
 * @param vbat 电池电压
 * @return 电量百分比 (0-100)
 */
int drv_battery_get_percent(float vbat);

/**
 * 读取ADC通道原始值（供按键等复用）
 * @param channel ADC通道
 * @param[out] out_raw 原始ADC值
 * @return ESP_OK 成功
 */
esp_err_t drv_battery_read_raw(adc_channel_t channel, int *out_raw);

#endif /* DRV_BATTERY_H */