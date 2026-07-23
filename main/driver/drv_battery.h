/**
 * @file drv_battery.h
 * @brief 电池驱动 - ADC采样和电量计算
 */

#ifndef DRV_BATTERY_H
#define DRV_BATTERY_H

/* ========== 电池ADC定义 ========== */
#define BAT_ADC_UNIT        ADC_UNIT_1
#define BAT_ADC_CHANNEL     ADC_CHANNEL_6
#define BAT_VDIV_R1         9100    // 上拉电阻 9.1k
#define BAT_VDIV_R2         2400    // 下拉电阻 2.4k
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

#endif /* DRV_BATTERY_H */