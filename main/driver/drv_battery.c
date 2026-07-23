/**
 * @file drv_battery.c
 * @brief 电池驱动实现
 */

#include "drv_battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "DRV_BAT";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_chars;

/* ========== 初始化电池ADC ========== */
void drv_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc_handle));
    
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg));
    
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = BAT_ADC_ATTEN,
        .bitwidth = BAT_ADC_BITWIDTH
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_cfg, &s_adc_chars));
    
    ESP_LOGI(TAG, "Battery ADC initialized (unit=%d, ch=%d)", BAT_ADC_UNIT, BAT_ADC_CHANNEL);
}

/* ========== 读取电池电压 ========== */
float drv_battery_get_voltage(void)
{
    int raw = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw));
    
    int mv = 0;
    ESP_ERROR_CHECK(adc_cali_raw_to_voltage(s_adc_chars, raw, &mv));
    
    // 分压计算：Vbat = Vadc * (R1 + R2) / R2
    float vadc = mv / 1000.0f;
    float vbat = vadc * (float)(BAT_VDIV_R1 + BAT_VDIV_R2) / (float)BAT_VDIV_R2;
    
    return vbat;
}

/* ========== 计算电池电量百分比 ========== */
int drv_battery_get_percent(float vbat)
{
    // 锂电池放电曲线近似：3.0V=0%, 4.2V=100%
    if (vbat >= 4.2f) return 100;
    if (vbat <= 3.0f) return 0;
    return (int)((vbat - 3.0f) / (4.2f - 3.0f) * 100.0f);
}