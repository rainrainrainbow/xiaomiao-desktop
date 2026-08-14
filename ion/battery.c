/**
 * @file ion/battery.c
 * @brief Ion - Hardware Abstraction Layer: Battery Implementation
 *
 * 电池管理驱动实现。
 * 使用 ESP32-S3 的 ADC1 通道读取电池电压（GPIO34 与 A 键共享）。
 * 支持分压计算、电量百分比估算、充电状态检测。
 */

#include "ion/battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ION_BAT";

/* ========== 硬件配置 ========== */
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_ADC_CHANNEL  ADC_CHANNEL_6   /* GPIO34 = ADC1_CH6 */
#define BAT_ADC_ATTEN    ADC_ATTEN_DB_11
#define BAT_ADC_BITWIDTH ADC_BITWIDTH_12

/* 分压电阻（参考硬件设计） */
#define BAT_VDIV_R1      100     /* 上拉电阻（kΩ） */
#define BAT_VDIV_R2      100     /* 下拉电阻（kΩ） */

/* 锂电池电压阈值 */
#define BAT_FULL_MV      4200    /* 满电 4.2V */
#define BAT_EMPTY_MV     3000    /* 亏电 3.0V */
#define BAT_CHARGING_MV  4300    /* 充电中电压 > 4.3V */
#define BAT_MIN_VALID_MV 2500    /* 有效电压下限 */

/* ========== 内部状态 ========== */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_chars = NULL;
static bool s_initialized = false;

/* 缓存状态 */
static uint16_t s_voltage_mv = 0;
static uint8_t s_percentage = 0;
static bool s_charging = false;
static char s_status_str[16];

/* ========== 初始化 ========== */

bool ion_battery_init(void)
{
    if (s_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing battery ADC");

    /* 初始化 ADC 单元 */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC unit: %d", ret);
        return false;
    }

    /* 配置 ADC 通道 */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH
    };
    ret = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC channel: %d", ret);
        return false;
    }

    /* 创建校准方案 */
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_chars);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ADC calibration: %d", ret);
        /* 校准失败不阻塞初始化，后续使用原始值估算 */
        s_adc_chars = NULL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Battery ADC initialized");
    return true;
}

/* ========== 读取电压 ========== */

uint16_t ion_battery_read_voltage_mv(void)
{
    if (!s_initialized || !s_adc_handle) {
        return 0;
    }

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %d", ret);
        return 0;
    }

    int mv = 0;
    if (s_adc_chars) {
        /* 使用校准值 */
        ret = adc_cali_raw_to_voltage(s_adc_chars, raw, &mv);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration failed, using raw estimate");
            mv = raw * 3300 / 4096;  /* 粗略估算 */
        }
    } else {
        /* 无校准时粗略估算 */
        mv = raw * 3300 / 4096;
    }

    /* 分压计算：Vbat = Vadc * (R1 + R2) / R2 */
    uint16_t vbat_mv = (uint16_t)((uint32_t)mv * (BAT_VDIV_R1 + BAT_VDIV_R2) / BAT_VDIV_R2);

    s_voltage_mv = vbat_mv;
    return vbat_mv;
}

/* ========== 电量百分比 ========== */

uint8_t ion_battery_get_percentage(void)
{
    uint16_t mv = ion_battery_read_voltage_mv();

    if (mv >= BAT_FULL_MV) {
        s_percentage = 100;
    } else if (mv <= BAT_EMPTY_MV) {
        s_percentage = 0;
    } else {
        /* 线性映射 3.0V-4.2V -> 0-100% */
        s_percentage = (uint8_t)((uint32_t)(mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV));
    }

    /* 充电中特殊处理 */
    if (mv >= BAT_CHARGING_MV) {
        s_charging = true;
        s_percentage = 100;
    } else {
        s_charging = false;
    }

    return s_percentage;
}

/* ========== 充电状态 ========== */

bool ion_battery_is_charging(void)
{
    uint16_t mv = ion_battery_read_voltage_mv();
    s_charging = (mv >= BAT_CHARGING_MV);
    return s_charging;
}

/* ========== 状态字符串 ========== */

const char* ion_battery_get_status_string(void)
{
    uint8_t pct = ion_battery_get_percentage();

    if (s_charging) {
        snprintf(s_status_str, sizeof(s_status_str), "充电中");
    } else if (pct < 10) {
        snprintf(s_status_str, sizeof(s_status_str), "低电量 %d%%", pct);
    } else {
        snprintf(s_status_str, sizeof(s_status_str), "%d%%", pct);
    }

    return s_status_str;
}

/* ========== 读取原始 ADC 值（供按键等复用） ========== */

esp_err_t ion_battery_read_raw(int *out_raw)
{
    if (!s_initialized || !s_adc_handle || !out_raw) {
        return ESP_ERR_INVALID_STATE;
    }
    return adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, out_raw);
}