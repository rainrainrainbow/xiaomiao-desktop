/**
 * @file app_sys_api.c
 * @brief 应用系统 API 门面层实现
 *
 * 本层只做"转发 + 统一"，不含业务逻辑。所有底层调用集中在此，
 * 新增 App 时无需再逐个 include 各底层驱动头文件。
 */

#include "app_sys_api.h"
#include "app_manager.h"
#include "esp_log.h"
#include "system/sys_nvs.h"
#include "driver/drv_battery.h"
#include "driver/drv_backlight.h"
#include "driver/drv_led_visualizer.h"
#include "ion/battery.h"
#include "ion/sdcard.h"
#include "lang/lang.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "APP_SYS_API";

/* ============================================================
 * 一、系统状态查询
 * ============================================================ */

uint8_t app_sys_battery_percent(void)
{
    return ion_battery_get_percentage();
}

uint16_t app_sys_battery_mv(void)
{
    return ion_battery_read_voltage_mv();
}

bool app_sys_sdcard_ready(void)
{
    return ion_sdcard_is_mounted();
}

bool app_sys_sdcard_try_mount(void)
{
    if (ion_sdcard_is_mounted()) return true;
    return ion_sdcard_init("/sdcard");
}

uint32_t app_sys_heap_internal_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t app_sys_psram_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

const char *app_sys_version(void)
{
    return XIAOMIAO_VERSION;
}

/* ============================================================
 * 二、用户配置读写
 * ============================================================ */

int app_sys_cfg_get_int(app_cfg_key_t key, int default_val)
{
    ui_state_t *st = ui_state_get();
    if (!st) return default_val;

    switch (key) {
    case APP_CFG_BRIGHTNESS:   return st->brightness;
    case APP_CFG_VOLUME:       return st->volume;
    case APP_CFG_SOUND_ON:     return st->sound_on ? 1 : 0;
    case APP_CFG_THEME:        return (int)st->theme;
    case APP_CFG_WIFI_ON:      return st->wifi_on ? 1 : 0;
    case APP_CFG_LAYOUT:       return st->layout;
    case APP_CFG_FONT_SIZE:    return st->font_size;
    case APP_CFG_SLEEP_TIMEOUT:return st->sleep_timeout;
    case APP_CFG_LANGUAGE:     return sys_nvs_load_language();
    case APP_CFG_FONT_SOURCE:  return sys_nvs_load_font_source();
    case APP_CFG_AUDIO_OUT:    return sys_nvs_load_audio_output();
    case APP_CFG_MUSIC_EQ:     return sys_nvs_load_music_eq();
    case APP_CFG_MUSIC_LRC:    return sys_nvs_load_music_lrc();
    case APP_CFG_MUSIC_MODE:   return sys_nvs_load_music_mode();
    case APP_CFG_LED_VIS_MODE: return led_visualizer_get_mode();
    case APP_CFG_LED_VIS_COLOR:return led_visualizer_get_color();
    default:                   return default_val;
    }
}

void app_sys_cfg_set_int(app_cfg_key_t key, int value)
{
    ui_state_t *st = ui_state_get();

    switch (key) {
    case APP_CFG_BRIGHTNESS:
        if (st) { st->brightness = value; drv_backlight_set_brightness(value); }
        break;
    case APP_CFG_VOLUME:
        if (st) st->volume = value;
        sys_nvs_save_volume(value);
        break;
    case APP_CFG_SOUND_ON:
        if (st) st->sound_on = (value != 0);
        break;
    case APP_CFG_THEME:
        if (st) { st->theme = (theme_type_t)value; ui_theme_set(st->theme); }
        break;
    case APP_CFG_WIFI_ON:
        if (st) st->wifi_on = (value != 0);
        break;
    case APP_CFG_LAYOUT:
        if (st) st->layout = value;
        break;
    case APP_CFG_FONT_SIZE:
        if (st) st->font_size = value;
        break;
    case APP_CFG_SLEEP_TIMEOUT:
        if (st) st->sleep_timeout = value;
        break;
    case APP_CFG_LANGUAGE:     sys_nvs_save_language(value); break;
    case APP_CFG_FONT_SOURCE:  sys_nvs_save_font_source(value); break;
    case APP_CFG_AUDIO_OUT:    sys_nvs_save_audio_output(value); break;
    case APP_CFG_MUSIC_EQ:     sys_nvs_save_music_eq(value); break;
    case APP_CFG_MUSIC_LRC:    sys_nvs_save_music_lrc(value); break;
    case APP_CFG_MUSIC_MODE:   sys_nvs_save_music_mode(value); break;
    case APP_CFG_LED_VIS_MODE: led_visualizer_set_mode(value); break;
    case APP_CFG_LED_VIS_COLOR:led_visualizer_set_color(value); break;
    default: break;
    }
}

void app_sys_cfg_save_all(void)
{
    ui_state_t *st = ui_state_get();
    if (!st) return;
    /* 核心 7 项批量持久化（与各设置子页现有行为一致） */
    sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                          (int)st->theme, st->wifi_on, st->layout, st->font_size);
}

/* ============================================================
 * 三、应用导航
 * ============================================================ */

void app_sys_nav_push(page_type_t type, const page_callbacks_t *cb, const char *title)
{
    if (!cb) return;
    ui_stack_push(type, cb, NULL);
    if (title) ui_statusbar_set_title(title);
}

void app_sys_nav_back(void)
{
    if (ui_stack_depth() > 1) ui_stack_pop();
}

void app_sys_nav_home(void)
{
    ui_stack_back_home();
}

void app_sys_set_title(const char *title)
{
    ui_statusbar_set_title(title);
}

/* ============================================================
 * 四、通用 UI 组件
 * ============================================================ */

lv_obj_t *app_sys_page_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        ESP_LOGE(TAG, "failed to create screen");
        return NULL;
    }
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const theme_colors_t *colors = ui_theme_colors();
    if (colors) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    }

    ui_statusbar_create(scr);
    if (title) ui_statusbar_set_title(title);
    return scr;
}

lv_obj_t *app_sys_list_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    lv_obj_t *list = lv_obj_create(parent);
    if (!list) return NULL;

    lv_obj_set_width(list, LCD_H_RES - 8);
    lv_obj_set_height(list, LCD_V_RES - ui_content_y() - DOCK_H - 4);
    lv_obj_set_x(list, 4);
    lv_obj_set_y(list, ui_content_y());
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 1, 0);

    const theme_colors_t *colors = ui_theme_colors();
    if (colors) {
        lv_obj_set_style_bg_color(list, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(colors->border), 0);
    }
    return list;
}

lv_obj_t *app_sys_list_add(lv_obj_t *list, const char *text)
{
    if (!list) return NULL;
    lv_obj_t *label = lv_label_create(list);
    if (!label) return NULL;
    lv_label_set_text(label, text ? text : "");
    return label;
}

void app_sys_list_highlight(lv_obj_t **rows, int count, int sel)
{
    if (!rows) return;
    const theme_colors_t *colors = ui_theme_colors();
    for (int i = 0; i < count; i++) {
        if (!rows[i]) continue;
        if (colors) {
            lv_obj_set_style_bg_color(rows[i],
                lv_color_hex(i == sel ? colors->sel_bg : colors->bg), 0);
            lv_obj_set_style_bg_opa(rows[i],
                i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

/* ============================================================
 * 五、文件/目录辅助
 * ============================================================ */

bool app_sys_fs_is_dir(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool app_sys_fs_is_file(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

bool app_sys_fs_has_ext(const char *name, const char *ext)
{
    if (!name || !ext) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    /* 大小写不敏感比较 */
    while (*dot && *ext) {
        char a = *dot, b = *ext;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
        dot++; ext++;
    }
    return *dot == '\0' && *ext == '\0';
}
