/**
 * @file sys_nvs.c
 * @brief NVS存储实现
 */

#include "sys_nvs.h"
#include "ui/ui_framework.h"
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

/* ========== 音频输出设备类型存储 ========== */
void sys_nvs_save_audio_output(int audio_out)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_AUDIO_OUT, audio_out);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Audio output saved: %d", audio_out);
}

int sys_nvs_load_audio_output(void)
{
    int32_t val = 0;
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_AUDIO_OUT, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Audio output loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Audio output not found, using default (0=auto)");
    return 0;
}

/* ========== 音频自动选择模式存储 ========== */
void sys_nvs_save_audio_auto(bool auto_mode)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_AUDIO_AUTO, auto_mode ? 1 : 0);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Audio auto mode saved: %d", auto_mode);
}

bool sys_nvs_load_audio_auto(void)
{
    int32_t val = 1;  // 默认自动模式
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_AUDIO_AUTO, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Audio auto mode loaded: %d", (int)val);
        return (val != 0);
    }
    ESP_LOGI(TAG, "Audio auto mode not found, using default (true=auto)");
    return true;
}

/* ========== 字库来源存储 ========== */
void sys_nvs_save_font_source(int font_source)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_FONT_SOURCE, font_source);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Font source saved: %d", font_source);
}

int sys_nvs_load_font_source(void)
{
    int32_t val = 0;  // 默认 FreeType
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_FONT_SOURCE, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Font source loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Font source not found, using default (0=FreeType)");
    return 0;
}

/* ========== 语言存储 ========== */
void sys_nvs_save_language(int lang)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_LANGUAGE, lang);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Language saved: %d", lang);
}

int sys_nvs_load_language(void)
{
    int32_t val = 0;  // 默认中文
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_LANGUAGE, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Language loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Language not found, using default (0=中文)");
    return 0;
}

/* ========== 字体文件路径索引存储 ========== */
void sys_nvs_save_font_path(int path_idx)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_FONT_PATH, path_idx);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Font path index saved: %d", path_idx);
}

int sys_nvs_load_font_path(void)
{
    int32_t val = 0;  // 默认自动
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_FONT_PATH, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Font path index loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Font path index not found, using default (0=auto)");
    return 0;
}

/* ========== 音量存储 ========== */
void sys_nvs_save_volume(int volume)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_VOLUME, volume);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Volume saved: %d", volume);
}

/* ========== 音乐设置存储 ========== */
void sys_nvs_save_music_eq(int enabled)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_MUSIC_EQ, enabled ? 1 : 0);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Music EQ saved: %d", enabled ? 1 : 0);
}

int sys_nvs_load_music_eq(void)
{
    int32_t val = 0;
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_MUSIC_EQ, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Music EQ loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Music EQ not found, using default (0=off)");
    return 0;
}

void sys_nvs_save_music_lrc(int enabled)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_MUSIC_LRC, enabled ? 1 : 0);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Music LRC saved: %d", enabled ? 1 : 0);
}

int sys_nvs_load_music_lrc(void)
{
    int32_t val = 0;
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_MUSIC_LRC, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Music LRC loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Music LRC not found, using default (0=off)");
    return 0;
}

void sys_nvs_save_music_mode(int mode)
{
    nvs_set_i32(s_nvs_handle, NVS_KEY_MUSIC_MODE, mode);
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "Music mode saved: %d", mode);
}

int sys_nvs_load_music_mode(void)
{
    int32_t val = 0;
    if (nvs_get_i32(s_nvs_handle, NVS_KEY_MUSIC_MODE, &val) == ESP_OK) {
        ESP_LOGI(TAG, "Music mode loaded: %d", (int)val);
        return (int)val;
    }
    ESP_LOGI(TAG, "Music mode not found, using default (0=single)");
    return 0;
}

void sys_nvs_save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid) {
        nvs_set_str(s_nvs_handle, NVS_KEY_WIFI_SSID, ssid);
    }
    if (password) {
        nvs_set_str(s_nvs_handle, NVS_KEY_WIFI_PASS, password);
    }
    nvs_commit(s_nvs_handle);
    ESP_LOGI(TAG, "WiFi credentials saved: SSID=%s", ssid ? ssid : "(null)");
}

bool sys_nvs_load_wifi_credentials(char *ssid, char *password)
{
    size_t ssid_len = 33;
    size_t pass_len = 65;
    
    esp_err_t err1 = nvs_get_str(s_nvs_handle, NVS_KEY_WIFI_SSID, ssid, &ssid_len);
    esp_err_t err2 = nvs_get_str(s_nvs_handle, NVS_KEY_WIFI_PASS, password, &pass_len);
    
    if (err1 == ESP_OK && err2 == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials loaded: SSID=%s", ssid);
        return true;
    }
    
    ESP_LOGI(TAG, "WiFi credentials not found in NVS");
    return false;
}