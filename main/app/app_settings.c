/**
 * @file app_settings.c
 * @brief 设置应用 + 关于系统子页面
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_settings_callbacks。
 * 参考 LiClock 的 App 架构设计，每个 App 独立文件。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "app_sys_api.h"      /* 系统状态/配置/导航门面层（版本、电池、内存等） */
#include "ui_framework.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "system/sys_nvs.h"
#include "driver/drv_backlight.h"
#include "driver/drv_audio_output.h"  /* 静音：无独立 mute API，用 set_volume(0) 生效 */
#include "esp_system.h"
#include "esp_timer.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"  /* lv_font_cn_get() 中文字体 */
#include "return_to_loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_SETTINGS";

/* ========== 设置应用（可滚动列表） ========== */
/* 行高根据字体大小动态计算，在 settings_init 中设置 */
static int s_settings_row_h = 14;
static int s_settings_vis_rows = 6;

/* 设置项：19项，分组显示 */
static const char *s_settings_items[] = {
    "亮度",       // 0 - 显示 → 二级页面  (deprecated, use lang_get)
    "主题",       // 1
    "音量",       // 2
    "WiFi",       // 3
    "布局",       // 4
    "字体",       // 5
    "字库选择",   // 6
    "语言",       // 7
    "声音",       // 8
    "音频输出",   // 9
    "屏幕超时",   // 10
    "日期时间",   // 11
    "OTA更新",    // 12
    "应用管理",   // 13
    "关于系统",   // 14
    "恢复默认",   // 15
    "保存并退出", // 16
    "返回Loader", // 17
    "蓝牙",       // 18 - 蓝牙设置（状态/配对）
    "恢复模式",   // 19 - 恢复模式（类安卓 Recovery 维护菜单）
    "LED音乐律动", // 20 - LED 随音乐律动（关/单色/彩虹/节拍脉冲）
};
#define SETTINGS_ITEM_COUNT (sizeof(s_settings_items) / sizeof(s_settings_items[0]))

/* 可见区域（列表起始Y由ui_content_y()动态计算） */

static lv_obj_t *s_settings_list = NULL;
static lv_obj_t *s_settings_labels[21] = {0};
static lv_obj_t *s_settings_switch = NULL;  /* 声音开关组件（第6行） */
static int s_settings_sel = 0;
static int s_settings_scroll = 0;  /* 滚动偏移（行数） */
/* 破坏性操作（恢复默认/返回Loader）的二次确认态：-1=无，>=0 为待确认菜单项索引。
 * 参考 app_recovery.c 的同类交互，避免误触清空设置或重启。 */
static int s_settings_confirm = -1;

/* 清除二次确认态并恢复设置页标题（移动光标/取消/退出确认时调用） */
static void settings_clear_confirm(void)
{
    if (s_settings_confirm >= 0) {
        s_settings_confirm = -1;
        ui_statusbar_set_title(lang_get(STR_SETTINGS));
    }
}

/* 声音开关生效：drv_audio_output 没有独立 mute API，
 * 关闭时用音量 0 实现静音，开启时恢复用户配置的音量。 */
static void settings_apply_sound(ui_state_t *st)
{
    audio_output_set_volume(st->sound_on ? (uint8_t)st->volume : 0);
}

static void settings_refresh_label(int idx)
{
    if (!s_settings_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[64];
    switch (idx) {
    case 0: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_BRIGHTNESS), st->brightness); break;
    case 1: snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_THEME),
                     st->theme == THEME_DARK ? lang_get(STR_THEME_DARK) : lang_get(STR_THEME_LIGHT)); break;
    case 2: snprintf(buf, sizeof(buf), "%s: %d%%", lang_get(STR_VOLUME), st->volume); break;
    case 3: snprintf(buf, sizeof(buf), "%s", lang_get(STR_WIFI)); break;
    case 4: snprintf(buf, sizeof(buf), "%s: %s",
                     lang_get(STR_LAYOUT), st->layout == 0 ? lang_get(STR_LAYOUT_3COL) : lang_get(STR_LAYOUT_2COL)); break;
    case 5: {
        const char *size_str = "14px";
        if (st->font_size == 16) size_str = "16px";
        else if (st->font_size == 20) size_str = "20px";
        else if (st->font_size == 24) size_str = "24px";
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_FONT), size_str);
        break;
    }
    case 6: {
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_FONT_SOURCE),
                 st->font_source == 0 ? lang_get(STR_FONT_ENGINE_FREETYPE) : lang_get(STR_FONT_SOURCE_BUILTIN));
        break;
    }
    case 7: {
        /* 语言：显示当前语言 */
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_LANGUAGE),
                 lang_get_current() == LANG_ZH ? lang_get(STR_LANGUAGE_ZH) : lang_get(STR_LANGUAGE_EN));
        break;
    }
    case 8: snprintf(buf, sizeof(buf), "%s", lang_get(STR_SOUND)); break;
    case 9: snprintf(buf, sizeof(buf), "%s", lang_get(STR_AUDIO_OUTPUT)); break;
    case 10: {
        const char *sleep_str = lang_get(STR_SLEEP_NEVER);
        if (st->sleep_timeout == 30) sleep_str = lang_get(STR_SLEEP_30S);
        else if (st->sleep_timeout == 60) sleep_str = lang_get(STR_SLEEP_60S);
        else if (st->sleep_timeout == 120) sleep_str = lang_get(STR_SLEEP_2M);
        else if (st->sleep_timeout == 300) sleep_str = lang_get(STR_SLEEP_5M);
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_SLEEP_TIMEOUT), sleep_str);
        break;
    }
    case 11: snprintf(buf, sizeof(buf), "%s", lang_get(STR_DATE_TIME)); break;
    case 12: snprintf(buf, sizeof(buf), "%s", lang_get(STR_OTA_IDLE)); break;
    case 13: snprintf(buf, sizeof(buf), "%s", lang_get(STR_APP_MANAGER)); break;
    case 14: snprintf(buf, sizeof(buf), "%s", lang_get(STR_ABOUT)); break;
    case 20: {
        /* LED音乐律动：显示当前模式 */
        const char *vis_names[] = {
            lang_get(STR_LED_VIS_OFF), lang_get(STR_LED_VIS_SINGLE),
            lang_get(STR_LED_VIS_RAINBOW), lang_get(STR_LED_VIS_PULSE)
        };
        int mode = app_sys_cfg_get_int(APP_CFG_LED_VIS_MODE, 0);
        if (mode < 0 || mode >= 4) mode = 0;
        snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_LED_VIS_TITLE), vis_names[mode]);
        break;
    }
    default: {
        const char *def_items[] = {
            lang_get(STR_BRIGHTNESS), lang_get(STR_THEME), lang_get(STR_VOLUME), lang_get(STR_WIFI),
            lang_get(STR_LAYOUT), lang_get(STR_FONT), lang_get(STR_FONT_SOURCE), lang_get(STR_LANGUAGE),
            lang_get(STR_SOUND), lang_get(STR_AUDIO_OUTPUT), lang_get(STR_SLEEP_TIMEOUT),
            lang_get(STR_DATE_TIME), lang_get(STR_OTA_IDLE), lang_get(STR_APP_MANAGER), lang_get(STR_ABOUT),
            lang_get(STR_RESET_DEFAULT), lang_get(STR_SAVE_EXIT), lang_get(STR_RETURN_LOADER),
            lang_get(STR_BLUETOOTH), lang_get(STR_RECOVERY), lang_get(STR_LED_VIS_TITLE)
        };
        snprintf(buf, sizeof(buf), "%s", idx >= 0 && idx < 21 ? def_items[idx] : "");
        break;
    }
    }
    lv_label_set_text(s_settings_labels[idx], buf);
}

/* 重建所有可见行的位置和显示状态 */
static void settings_rebuild_visible(void)
{
    if (!s_settings_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int vis_rows = s_settings_vis_rows;
    if (vis_rows < 1) vis_rows = 1;
    /* 清除所有子对象 */
    lv_obj_clean(s_settings_list);
    memset(s_settings_labels, 0, sizeof(s_settings_labels));
    s_settings_switch = NULL;
    /* 只创建可见范围内的行 */
    for (int i = 0; i < vis_rows && (s_settings_scroll + i) < SETTINGS_ITEM_COUNT; i++) {
        int idx = s_settings_scroll + i;
        lv_obj_t *row = lv_obj_create(s_settings_list);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_settings_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_settings_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_settings_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! idx=%d mem free=%lu", idx,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        /* 设置页面的字体根据 font_size 自适应 */
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 12);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_settings_labels[idx] = lbl;
        settings_refresh_label(idx);

        /* 第8行（声音开关）：添加LVGL开关组件 */
        if (idx == 8) {
            lv_obj_t *sw = lv_switch_create(row);
            if (!sw) {
                ESP_LOGE(TAG, "lv_switch_create(sw) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_obj_remove_style_all(sw);
            /* 开关背景 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->border), 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(sw, 8, 0);
            /* 开关指示器（开启时填充色） */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->text), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(sw, 8, LV_PART_INDICATOR);
            /* 开关旋钮 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->bg), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
            lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
            lv_obj_set_size(sw, 36, s_settings_row_h - 4);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
            /* 设置开关状态 */
            if (st->sound_on) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
            }
            s_settings_switch = sw;
        }
    }
}

static void settings_scroll_to_sel(void)
{
    /* 确保选中项在可见范围内 */
    if (s_settings_sel < s_settings_scroll) {
        s_settings_scroll = s_settings_sel;
    } else if (s_settings_sel >= s_settings_scroll + s_settings_vis_rows) {
        s_settings_scroll = s_settings_sel - s_settings_vis_rows + 1;
    }
    /* 限制滚动范围 */
    if (s_settings_scroll > SETTINGS_ITEM_COUNT - s_settings_vis_rows) {
        s_settings_scroll = SETTINGS_ITEM_COUNT - s_settings_vis_rows;
    }
    if (s_settings_scroll < 0) s_settings_scroll = 0;
}

static void settings_init(void *data)
{
    ESP_LOGI(TAG, "Settings app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_SETTINGS));
    
    /* 根据字体大小动态计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_settings_row_h = font_px + 2;  /* 字体高度 + 2px间距 */
    /* 可见行数根据实际内容区高度（ui_content_y）计算 */
    s_settings_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_settings_row_h;
    if (s_settings_vis_rows < 1) s_settings_vis_rows = 1;
    
    /* 列表起始位置：状态栏下方 */
    lv_coord_t list_y = ui_content_y();
    
    s_settings_list = lv_obj_create(scr);
    if (!s_settings_list) {
        ESP_LOGE(TAG, "lv_obj_create(settings_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_settings_list);
    lv_obj_set_pos(s_settings_list, 0, list_y);
    lv_obj_set_size(s_settings_list, LCD_H_RES, LCD_V_RES - list_y - DOCK_H);
    lv_obj_clear_flag(s_settings_list, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_sel = 0;
    s_settings_scroll = 0;
    s_settings_confirm = -1;
    /* 进入设置页时若处于静音态，确保驱动层也静音；非静音时不覆盖当前音量 */
    if (!st->sound_on) audio_output_set_volume(0);
    settings_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void settings_activate(void)
{
    ESP_LOGI(TAG, "Settings app activate");
    ui_statusbar_set_title(lang_get(STR_SETTINGS));
    /* 避免重复 rebuild：settings_init() 已经调用过 settings_rebuild_visible()
     * 重复调用会导致标签被销毁重建，触发 LVGL v9.5 glyph cache 的 use-after-free bug
     * （lv_cache_lru_rb.c 的 get_victim_cb 在 evict 时访问已释放的节点）
     * 只在列表不存在时才 rebuild（例如从其他页面返回时）
     */
    if (!s_settings_list) {
        settings_rebuild_visible();
    }
}

static void settings_destroy(void)
{
    ESP_LOGI(TAG, "Settings app destroy");
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) s_settings_labels[i] = NULL;
    s_settings_list = NULL;
    s_settings_switch = NULL;
    s_settings_confirm = -1;
}

static bool settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();
    if (key == KEY_B) {
        /* 二次确认态下：B 仅取消本次确认，不退出设置页 */
        if (s_settings_confirm >= 0) {
            settings_clear_confirm();
            return true;
        }
        if (ui_stack_depth() > 1) {
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            sys_nvs_save_font_source(st->font_source);
            ui_stack_pop();
        }
        return true;
    }
    if (key == KEY_UP) {
        settings_clear_confirm();  /* 移动光标即取消待确认操作 */
        s_settings_sel = (s_settings_sel - 1 + SETTINGS_ITEM_COUNT) % SETTINGS_ITEM_COUNT;
        settings_scroll_to_sel();
        settings_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        settings_clear_confirm();  /* 移动光标即取消待确认操作 */
        s_settings_sel = (s_settings_sel + 1) % SETTINGS_ITEM_COUNT;
        settings_scroll_to_sel();
        settings_rebuild_visible();
        return true;
    }
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_A) {
        switch (s_settings_sel) {
        case 0:
            /* 亮度 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_brightness_settings_callbacks, NULL);
                return true;
            }
            break;
        case 1:
            /* 主题 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_theme_settings_callbacks, NULL);
                return true;
            }
            break;
        case 2:
            /* 音量 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_volume_settings_callbacks, NULL);
                return true;
            }
            break;
        case 3:
            /* WiFi → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_wifi_settings_callbacks, NULL);
                return true;
            }
            break;
        case 4:
            /* 布局 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_layout_settings_callbacks, NULL);
                return true;
            }
            break;
        case 5:
            /* 字体 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_font_settings_callbacks, NULL);
                return true;
            }
            break;
        case 6:
            /* 字库选择 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_font_source_settings_callbacks, NULL);
                return true;
            }
            break;
        case 7:
            /* 语言 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_language_settings_callbacks, NULL);
                return true;
            }
            break;
        case 8:
            /* 声音开关（含lv_switch） */
            if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_A) {
                st->sound_on = !st->sound_on;
                settings_apply_sound(st);  /* 真正驱动静音/恢复音量 */
                if (s_settings_switch) {
                    if (st->sound_on) {
                        lv_obj_add_state(s_settings_switch, LV_STATE_CHECKED);
                    } else {
                        lv_obj_clear_state(s_settings_switch, LV_STATE_CHECKED);
                    }
                }
            }
            break;
        case 9:
            /* 音频输出 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_audio_settings_callbacks, NULL);
                return true;
            }
            break;
        case 10:
            /* 屏幕超时 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_sleep_settings_callbacks, NULL);
                return true;
            }
            break;
        case 11:
            /* 日期时间 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_datetime_settings_callbacks, NULL);
                return true;
            }
            break;
        case 12:
            /* OTA更新 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_ota_callbacks, NULL);
                return true;
            }
            break;
        case 13:
            /* 应用管理 → 二级页面 */
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_applist_callbacks, NULL);
            return true;
        case 14:
            /* 关于系统 → 二级页面 */
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_about_callbacks, NULL);
            return true;
        case 15:
            /* 恢复默认（破坏性）：二次确认；LEFT/RIGHT 不触发，仅 A 键生效 */
            if (key == KEY_A) {
                if (s_settings_confirm == 15) {
                    /* 第二次 A：执行恢复默认 */
                    s_settings_confirm = -1;
                    ui_statusbar_set_title(lang_get(STR_SETTINGS));
                    st->brightness = 50; st->volume = 50; st->theme = THEME_DARK;
                    st->sound_on = true; st->wifi_on = false; st->layout = 0; st->font_size = 14;
                    st->sleep_timeout = 60; st->font_source = 0;
                    /* 清空用户选择的字体路径并立即持久化，确保重启后恢复默认字体 */
                    sys_nvs_save_font_path(0);
                    sys_nvs_save_font_source(0);
                    drv_backlight_set_brightness(st->brightness);
                    ui_theme_set(st->theme);
                    ESP_LOGI(TAG, "Settings reset to defaults");
                    settings_rebuild_visible();
                } else {
                    /* 第一次 A：进入确认态，状态栏提示再按一次 + 清除范围说明。
                     * 状态栏标题为循环滚动标签，长文本会滚动显示；用 static 缓冲避免栈开销。 */
                    static char s_confirm_title[96];
                    s_settings_confirm = 15;
                    snprintf(s_confirm_title, sizeof(s_confirm_title), "%s %s",
                             lang_get(STR_REC_CONFIRM), lang_get(STR_SET_FACTORY_SCOPE));
                    ui_statusbar_set_title(s_confirm_title);
                }
            }
            return true;
        case 16:
            /* 保存并退出 */
            sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                  (int)st->theme, st->wifi_on, st->layout, st->font_size);
            sys_nvs_save_font_source(st->font_source);
            ui_stack_pop();
            return true;
        case 17:
            /* 返回Loader（会重启，破坏性）：二次确认；LEFT/RIGHT 不触发，仅 A 键生效 */
            if (key == KEY_A) {
                if (s_settings_confirm == 17) {
                    /* 第二次 A：执行返回 Loader */
                    s_settings_confirm = -1;
                    ESP_LOGI(TAG, "Returning to loader (download mode)...");
                    sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                                          (int)st->theme, st->wifi_on, st->layout, st->font_size);
                    sys_nvs_save_font_source(st->font_source);
                    /* 显式设置下次启动到 factory（Loader）分区，然后重启 */
                    return_to_loader_setup();
                    esp_restart();
                } else {
                    /* 第一次 A：进入确认态，状态栏提示再按一次 */
                    s_settings_confirm = 17;
                    ui_statusbar_set_title(lang_get(STR_REC_CONFIRM));
                }
            }
            return true;
        case 18:
            /* 蓝牙 → 二级页面 */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_bt_settings_callbacks, NULL);
                return true;
            }
            break;
        case 19:
            /* 恢复模式 → 二级页面（类安卓 Recovery 维护菜单） */
            if (key == KEY_A) {
                ui_stack_push(PAGE_APP_PLACEHOLDER, &g_recovery_callbacks, NULL);
                return true;
            }
            break;
        case 20:
            /* LED音乐律动：A/左右键循环切换模式 关→单色→彩虹→节拍 */
            if (key == KEY_A || key == KEY_LEFT || key == KEY_RIGHT) {
                int mode = app_sys_cfg_get_int(APP_CFG_LED_VIS_MODE, 0);
                int delta = (key == KEY_LEFT) ? -1 : 1;
                mode = (mode + delta + 4) % 4;   /* 4 种模式循环 */
                app_sys_cfg_set_int(APP_CFG_LED_VIS_MODE, mode);
                ESP_LOGI(TAG, "LED visualizer mode -> %d", mode);
            }
            break;
        }
        settings_refresh_label(s_settings_sel);
        return true;
    }
    return false;
}

/* ========== 关于系统页面（摘要 / 详情分层，可滚动） ==========
 * 摘要页：版本号 / 设备名 / 内部存储 / 内存卡 / 电量 / 开机时长 + 详细信息入口。
 * 详情页：设备信息 / 硬件信息 / 存储信息 / 网络信息 / 系统状态 /
 *        软件许可 / 用户协议 / 支持与反馈 / 快捷入口 分组显示，A 键进 OTA。
 * 行文本统一格式化进 static 行缓冲，避免栈上大数组。 */
#define ABOUT_MAX_ROWS      48
#define ABOUT_ACTION_DETAIL 1   /* 摘要页：进入详情页 */
#define ABOUT_ACTION_OTA    2   /* 详情页：检查更新 → OTA */

/* 未配置时默认 240MHz（ESP32 主频） */
#ifndef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#endif
/* 兼容个别 LVGL 发行包未导出 PATCH 宏的情况 */
#ifndef LVGL_VERSION_PATCH
#define LVGL_VERSION_PATCH 0
#endif

/* 行类型：缩进数据行 / 分组标题 / 可操作项 */
typedef enum {
    ABOUT_ROW_TEXT = 0,
    ABOUT_ROW_GROUP,
    ABOUT_ROW_ACTION,
} about_row_kind_t;

typedef struct {
    uint8_t kind;    /* about_row_kind_t */
    uint8_t action;  /* ABOUT_ACTION_xxx，仅 ACTION 行有效 */
    char text[72];   /* 已格式化好的显示文本 */
} about_row_t;

/* 视图模式 */
typedef enum {
    ABOUT_VIEW_SUMMARY = 0,
    ABOUT_VIEW_DETAIL,
} about_view_t;

static about_row_t s_about_rows[ABOUT_MAX_ROWS];   /* static 缓冲：行文本直接格式化至此 */
static char s_about_scratch[32];                    /* static 临时缓冲：chip_id / MAC / IP */
static int s_about_row_count = 0;
static about_view_t s_about_view = ABOUT_VIEW_SUMMARY;
static int s_about_sel = 0;      /* 当前选中行的逻辑索引 */
static int s_about_scroll = 0;   /* 滚动偏移（逻辑行） */
/* 行高根据字体大小动态计算，在 about_init 中设置 */
static int s_about_row_h = 14;
static int s_about_vis_rows = 6;
static lv_obj_t *s_about_obj = NULL;

/* 追加一行，返回可写入 text 的行指针（满则返回 NULL） */
static about_row_t *about_row_add(uint8_t kind, uint8_t action)
{
    if (s_about_row_count >= ABOUT_MAX_ROWS) return NULL;
    about_row_t *r = &s_about_rows[s_about_row_count++];
    r->kind = kind;
    r->action = action;
    r->text[0] = '\0';
    return r;
}

/* 格式化追加一行（文本最终落在 static 行缓冲里） */
static void about_add_fmt(uint8_t kind, uint8_t action, const char *fmt, ...)
{
    about_row_t *r = about_row_add(kind, action);
    if (!r) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->text, sizeof(r->text), fmt, ap);
    va_end(ap);
}

/* 电量：统一走系统门面层，有效性阈值 2200mV（门面层不含二极管压降） */
static void about_battery_text(char *dst, size_t n)
{
    uint16_t mv = app_sys_battery_mv();
    if (mv >= 2200u) {
        snprintf(dst, n, "%d%%  %.2fV", app_sys_battery_percent(), mv / 1000.0f);
    } else {
        snprintf(dst, n, "N/A");
    }
}

/* 构建当前视图的所有行 */
static void about_build_rows(void)
{
    s_about_row_count = 0;

    if (s_about_view == ABOUT_VIEW_SUMMARY) {
        /* ---- 摘要页：首屏一屏内可见的关键信息 ---- */
        about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_VERSION), app_sys_version());
        about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: XiaoMiaoOS", lang_get(STR_SYSTEM));
        about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luk/%luk",
                      lang_get(STR_ABOUT_FLASH_TOTAL),
                      (unsigned long)app_sys_flash_free_kb(),
                      (unsigned long)app_sys_flash_total_kb());
        {
            uint32_t sd_kb = app_sys_sdcard_total_kb();
            if (sd_kb > 0) {
                about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luk/%luk",
                              lang_get(STR_ABOUT_SD_TOTAL),
                              (unsigned long)app_sys_sdcard_free_kb(), (unsigned long)sd_kb);
            } else {
                about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: N/A", lang_get(STR_ABOUT_SD_TOTAL));
            }
        }
        about_battery_text(s_about_scratch, sizeof(s_about_scratch));
        about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_BATTERY), s_about_scratch);
        {
            uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
            about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luh%lum", lang_get(STR_UPTIME),
                          (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60));
        }
        about_add_fmt(ABOUT_ROW_ACTION, ABOUT_ACTION_DETAIL, "%s  >", lang_get(STR_ABOUT_DETAIL));
        return;
    }

    /* ---- 详情页：分组标题 + 缩进条目 ---- */
    /* 1. 设备信息 */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_SUMMARY));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "型号: %s", app_sys_chip_model());
    app_sys_chip_id(s_about_scratch, sizeof(s_about_scratch));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_ABOUT_SERIAL), s_about_scratch);
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: V1.0", lang_get(STR_ABOUT_HWVER));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_BUILD), XIAOMIAO_BUILD);

    /* 2. 硬件信息（"硬件信息/系统状态/快捷入口"暂无独立 lang 字符串，沿用中文标题） */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", "硬件信息");
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_CHIP), app_sys_chip_model());
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %d MHz", lang_get(STR_CPU_FREQ), CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %lu/%luK", lang_get(STR_MEMORY),
                  (unsigned long)(app_sys_heap_internal_free() / 1024),
                  (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: ST7735 160x128", lang_get(STR_SCREEN));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "音频芯片: NS4168");
    about_add_fmt(ABOUT_ROW_TEXT, 0, "LED数量: 3");

    /* 3. 存储信息 */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_STORAGE));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luk/%luk", lang_get(STR_ABOUT_FLASH_TOTAL),
                  (unsigned long)app_sys_flash_free_kb(), (unsigned long)app_sys_flash_total_kb());
    {
        uint32_t sd_kb = app_sys_sdcard_total_kb();
        if (sd_kb > 0) {
            about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luk/%luk", lang_get(STR_ABOUT_SD_TOTAL),
                          (unsigned long)app_sys_sdcard_free_kb(), (unsigned long)sd_kb);
        } else {
            about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: N/A", lang_get(STR_ABOUT_SD_TOTAL));
        }
    }

    /* 4. 网络信息 */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_NETWORK));
    app_sys_net_mac(s_about_scratch, sizeof(s_about_scratch));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_ABOUT_MAC), s_about_scratch);
    app_sys_net_ip(s_about_scratch, sizeof(s_about_scratch));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_ABOUT_IPV4), s_about_scratch);
    {
        int8_t rssi = app_sys_net_rssi();
        if (rssi != 0) {
            about_add_fmt(ABOUT_ROW_TEXT, 0, "RSSI: %d dBm", rssi);
        } else {
            about_add_fmt(ABOUT_ROW_TEXT, 0, "RSSI: N/A");
        }
    }

    /* 5. 系统状态 */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", "系统状态");
    {
        uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luh%lum", lang_get(STR_UPTIME),
                      (unsigned long)(sec / 3600), (unsigned long)((sec % 3600) / 60));
    }
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %luk", lang_get(STR_MEMORY),
                  (unsigned long)(app_sys_heap_internal_free() / 1024));
    about_battery_text(s_about_scratch, sizeof(s_about_scratch));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_BATTERY), s_about_scratch);
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %s", lang_get(STR_SDK_VERSION), app_sys_sdk_version());
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: %d.%d.%d", lang_get(STR_LVGL_VERSION),
                  LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: N/A", lang_get(STR_ABOUT_TEMP));  /* 无温度传感器 */
    about_add_fmt(ABOUT_ROW_TEXT, 0, "%s: N/A", lang_get(STR_ABOUT_CPU));   /* 无 CPU 占用率 API */

    /* 6~8. 软件许可 / 用户协议 / 支持与反馈（暂无正文内容，占位说明） */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_LICENSE));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "ESP-IDF / LVGL (MIT)");
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_EULA));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "XiaoMiaoOS");
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", lang_get(STR_ABOUT_SUPPORT));
    about_add_fmt(ABOUT_ROW_TEXT, 0, "N/A");

    /* 9. 快捷入口：检查更新 → OTA 页 */
    about_add_fmt(ABOUT_ROW_GROUP, 0, "%s", "快捷入口");
    about_add_fmt(ABOUT_ROW_ACTION, ABOUT_ACTION_OTA, "%s  >", lang_get(STR_ABOUT_CHECKUPDATE));
}

/* 仅重建可见区域的行：分组标题用次要色，数据行缩进，选中行高亮 */
static void about_rebuild_visible(void)
{
    if (!s_about_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_about_obj);
    int vis = s_about_vis_rows;
    if (vis < 1) vis = 1;
    for (int i = 0; i < vis && (s_about_scroll + i) < s_about_row_count; i++) {
        int idx = s_about_scroll + i;
        about_row_t *row = &s_about_rows[idx];
        lv_obj_t *r = lv_obj_create(s_about_obj);
        if (!r) {
            ESP_LOGE(TAG, "lv_obj_create(about_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 0, i * s_about_row_h);
        lv_obj_set_size(r, LCD_H_RES, s_about_row_h);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_about_sel) {
            lv_obj_set_style_bg_color(r, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(r);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(about_lbl) failed! idx=%d mem free=%lu", idx,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        /* 分组标题用次要色，数据行缩进，操作项用主文本色 */
        uint8_t indent = 6;
        if (row->kind == ABOUT_ROW_GROUP) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            if (row->kind == ABOUT_ROW_TEXT) indent = 12;
        }
        /* 关于页面的字体根据 font_size 自适应 */
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_set_width(lbl, LCD_H_RES - indent - 6);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, indent, 0);
        lv_label_set_text(lbl, row->text);
    }
}

/* 确保选中行在可见范围内 */
static void about_scroll_to_sel(void)
{
    if (s_about_sel < s_about_scroll) {
        s_about_scroll = s_about_sel;
    } else if (s_about_sel >= s_about_scroll + s_about_vis_rows) {
        s_about_scroll = s_about_sel - s_about_vis_rows + 1;
    }
    if (s_about_scroll > s_about_row_count - s_about_vis_rows) {
        s_about_scroll = s_about_row_count - s_about_vis_rows;
    }
    if (s_about_scroll < 0) s_about_scroll = 0;
}

/* 切换摘要/详情视图：重建行数据并回到顶部 */
static void about_enter_view(about_view_t view)
{
    s_about_view = view;
    s_about_sel = 0;
    s_about_scroll = 0;
    about_build_rows();
    ui_statusbar_set_title(view == ABOUT_VIEW_SUMMARY ? lang_get(STR_ABOUT) : lang_get(STR_ABOUT_DETAIL));
    about_rebuild_visible();
}

static void about_init(void *data)
{
    ESP_LOGI(TAG, "About page init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);

    /* 根据字体大小动态计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_about_row_h = font_px + 2;  /* 字体高度 + 2px间距 */
    /* 可见行数根据实际内容区高度（ui_content_y）计算 */
    s_about_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_about_row_h;
    if (s_about_vis_rows < 1) s_about_vis_rows = 1;

    /* 列表起始位置：状态栏下方 */
    lv_coord_t list_y = ui_content_y();

    s_about_obj = lv_obj_create(scr);
    if (!s_about_obj) {
        ESP_LOGE(TAG, "lv_obj_create(about_obj) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_about_obj);
    lv_obj_set_pos(s_about_obj, 0, list_y);
    lv_obj_set_size(s_about_obj, LCD_H_RES, LCD_V_RES - list_y - DOCK_H);
    lv_obj_clear_flag(s_about_obj, LV_OBJ_FLAG_SCROLLABLE);

    /* 默认进入摘要页（构建行数据 + 渲染 + 设置标题） */
    about_enter_view(ABOUT_VIEW_SUMMARY);
    ui_dock_create(scr, 1, 0);
}

static void about_destroy(void)
{
    ESP_LOGI(TAG, "About page destroy");
    s_about_obj = NULL;
    /* 回到关于页时重置为摘要视图，避免残留详情页的滚动位置 */
    s_about_view = ABOUT_VIEW_SUMMARY;
    s_about_row_count = 0;
    s_about_sel = 0;
    s_about_scroll = 0;
}

static bool about_on_key(int key)
{
    if (key == KEY_B) {
        if (s_about_view == ABOUT_VIEW_DETAIL) {
            /* 详情页：B 返回摘要页（再按 B 才退出关于页） */
            about_enter_view(ABOUT_VIEW_SUMMARY);
        } else if (ui_stack_depth() > 1) {
            ui_stack_pop();
        }
        return true;
    }
    if (key == KEY_UP) {
        if (s_about_sel > 0) {
            s_about_sel--;
            about_scroll_to_sel();
            about_rebuild_visible();
        }
        return true;
    }
    if (key == KEY_DOWN) {
        if (s_about_sel + 1 < s_about_row_count) {
            s_about_sel++;
            about_scroll_to_sel();
            about_rebuild_visible();
        }
        return true;
    }
    if (key == KEY_A) {
        about_row_t *row = (s_about_sel >= 0 && s_about_sel < s_about_row_count)
                         ? &s_about_rows[s_about_sel] : NULL;
        if (row && row->kind == ABOUT_ROW_ACTION) {
            if (row->action == ABOUT_ACTION_DETAIL) {
                about_enter_view(ABOUT_VIEW_DETAIL);
            } else if (row->action == ABOUT_ACTION_OTA) {
                /* 快捷入口：跳转到 OTA 更新页（与设置页 OTA 项同款注册方式） */
                app_sys_nav_push(PAGE_APP_PLACEHOLDER, &g_ota_callbacks, NULL);
            }
        }
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_settings_callbacks = {
    .init = settings_init,
    .activate = settings_activate,
    .destroy = settings_destroy,
    .on_key = settings_on_key,
};

const page_callbacks_t g_about_callbacks = {
    .init = about_init,
    .destroy = about_destroy,
    .on_key = about_on_key,
};