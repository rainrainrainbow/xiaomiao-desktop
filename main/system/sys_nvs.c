/**
 * @file sys_nvs.c
 * @brief NVS存储实现
 */

#include "sys_nvs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "SYS_NVS";
static nvs_handle_t s_nvs_handle;

/* ========== 初始化NVS存储 ========== */
int sys_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init error, erasing: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "NVS initialized (namespace=%s)", NVS_NAMESPACE);
    return 0;
}

/* ========== 保存设置到NVS ========== */
void sys_nvs_save_settings(int brightness, int volume, bool sound_on, int theme,
                           bool wifi_on, int layout, int font_size)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_BRIGHTNESS, brightness);
    nvs_set_i32(s_nvs_handle, NVS_KEY_VOLUME, volume);
    nvs_set_i32(s_nvs_handle, NVS_KEY_SOUND, sound_on ? 1 : 0);
    nvs_set_i32(s_nvs_handle, NVS_KEY_THEME, theme);
    nvs_set_i32(s_nvs_handle, NVS_KEY_WIFI, wifi_on ? 1 : 0);
    nvs_set_i32(s_nvs_handle, NVS_KEY_LAYOUT, layout);
    nvs_set_i32(s_nvs_handle, NVS_KEY_FONT_SIZE, font_size);
    /* 从ui_state_t读取sleep_timeout并保存 */
    extern ui_state_t* ui_state_get(void);
    nvs_set_i32(s_nvs_handle, NVS_KEY_SLEEP, ui_state_get()->sleep_timeout);
    nvs_commit(s_nvs_handle);
    
    ESP_LOGI(TAG, "Settings saved: brightness=%d, volume=%d, sound=%d, theme=%d, wifi=%d, layout=%d, font_size=%d",
             brightness, volume, sound_on, theme, wifi_on, layout, font_size);
}

/* ========== 从NVS加载设置 ========== */
bool sys_nvs_load_settings(int *brightness, int *volume, bool *sound_on, int *theme,
                           bool *wifi_on, int *layout, int *font_size)
{
    int32_t val;
    bool loaded = false;
    
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_BRIGHTNESS, &val) == ESP_OK) {
        *brightness = val;
        loaded = true;
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_VOLUME, &val) == ESP_OK) {
        *volume = val;
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_SOUND, &val) == ESP_OK) {
        *sound_on = (val != 0);
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_THEME, &val) == ESP_OK) {
        *theme = val;
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_WIFI, &val) == ESP_OK) {
        *wifi_on = (val != 0);
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_LAYOUT, &val) == ESP_OK) {
        *layout = val;
    }
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_FONT_SIZE, &val) == ESP_OK) {
        *font_size = val;
    }
    /* 加载sleep_timeout */
    extern ui_state_t* ui_state_get(void);
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_SLEEP, &val) == ESP_OK) {
        ui_state_get()->sleep_timeout = val;
    }
    
    if (loaded) {
        ESP_LOGI(TAG, "Settings loaded: brightness=%d, volume=%d, sound=%d, theme=%d, wifi=%d, layout=%d, font_size=%d",
                 *brightness, *volume, *sound_on, *theme, *wifi_on, *layout, *font_size);
    } else {
        ESP_LOGI(TAG, "No saved settings, using defaults");
    }
    
    return loaded;
}