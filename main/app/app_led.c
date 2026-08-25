/**
 * @file app_led.c
 * @brief 灯效应用 — 控制 WS2812B LED 灯带效果
 *
 * 支持效果：
 * - 关闭
 * - 单色常亮（可调色）
 * - 呼吸灯（可调色）
 * - 彩虹渐变
 * - 流光溢彩
 * - 闪烁
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "driver/drv_led_strip.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include <string.h>
#include <math.h>

static const char *TAG = "APP_LED";

/* ========== 效果定义 ========== */
typedef enum {
    LED_EFFECT_OFF = 0,        // 关闭
    LED_EFFECT_STATIC,         // 单色常亮
    LED_EFFECT_BREATH,         // 呼吸灯
    LED_EFFECT_RAINBOW,        // 彩虹渐变
    LED_EFFECT_FLOW,           // 流光溢彩
    LED_EFFECT_BLINK,          // 闪烁
    LED_EFFECT_COUNT
} led_effect_t;

static const char *s_effect_names_zh[] = {
    "关闭",
    "单色常亮",
    "呼吸灯",
    "彩虹渐变",
    "流光溢彩",
    "闪烁",
};

/* 预设颜色 */
typedef struct {
    const char *name;
    led_rgb_t color;
} color_preset_t;

static const color_preset_t s_color_presets[] = {
    {"红",    {.r=255, .g=0,   .b=0}},
    {"橙",    {.r=255, .g=128, .b=0}},
    {"黄",    {.r=255, .g=255, .b=0}},
    {"绿",    {.r=0,   .g=255, .b=0}},
    {"青",    {.r=0,   .g=255, .b=255}},
    {"蓝",    {.r=0,   .g=0,   .b=255}},
    {"紫",    {.r=128, .g=0,   .b=255}},
    {"粉",    {.r=255, .g=0,   .b=128}},
    {"白",    {.r=255, .g=255, .b=255}},
};
#define COLOR_PRESET_COUNT (sizeof(s_color_presets) / sizeof(s_color_presets[0]))

/* ========== 全局状态 ========== */
static lv_obj_t *s_led_obj = NULL;       // 列表容器
static lv_obj_t *s_info_label = NULL;    // 信息提示标签
static int s_sel = 0;                    // 选中索引
static int s_scroll = 0;                 // 滚动偏移
static int s_row_h = 16;                 // 行高

/* 当前LED状态 */
static led_effect_t s_current_effect = LED_EFFECT_OFF;
static int s_current_color = 0;          // 选中颜色预设索引
static uint8_t s_current_brightness = 128; // 当前亮度

/* 效果任务相关 */
static bool s_effect_running = false;
static volatile bool s_effect_stop = false;

/* ========== 颜色辅助函数 ========== */

/** HSV 转 RGB */
static led_rgb_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    led_rgb_t rgb = {0, 0, 0};
    uint8_t region, remainder, p, q, t;
    
    if (s == 0) {
        rgb.r = rgb.g = rgb.b = v;
        return rgb;
    }
    
    region = h / 43;
    remainder = (h - region * 43) * 6;
    
    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0: rgb.r = v; rgb.g = t; rgb.b = p; break;
        case 1: rgb.r = q; rgb.g = v; rgb.b = p; break;
        case 2: rgb.r = p; rgb.g = v; rgb.b = t; break;
        case 3: rgb.r = p; rgb.g = q; rgb.b = v; break;
        case 4: rgb.r = t; rgb.g = p; rgb.b = v; break;
        default: rgb.r = v; rgb.g = p; rgb.b = q; break;
    }
    return rgb;
}

/* ========== 效果任务函数 ========== */

/** 应用当前选中效果（在任务中运行） */
static void led_effect_task(void *arg)
{
    (void)arg;
    s_effect_running = true;
    s_effect_stop = false;
    
    led_rgb_t base_color = s_color_presets[s_current_color].color;
    
    switch (s_current_effect) {
    case LED_EFFECT_OFF:
        drv_led_strip_clear();
        break;
        
    case LED_EFFECT_STATIC:
        drv_led_strip_set_all(base_color);
        drv_led_strip_refresh();
        break;
        
    case LED_EFFECT_BREATH:
        while (!s_effect_stop) {
            for (int phase = 0; phase < 256 && !s_effect_stop; phase += 4) {
                float rad = (float)phase * 3.14159f / 128.0f;
                uint8_t breath = (uint8_t)(128 + 127 * sinf(rad));
                led_rgb_t scaled = {
                    .r = base_color.r * breath / 255,
                    .g = base_color.g * breath / 255,
                    .b = base_color.b * breath / 255,
                };
                drv_led_strip_set_all(scaled);
                drv_led_strip_refresh();
                vTaskDelay(pdMS_TO_TICKS(30));
            }
        }
        break;
        
    case LED_EFFECT_RAINBOW: {
        uint16_t hue = 0;
        while (!s_effect_stop) {
            led_rgb_t color = hsv_to_rgb(hue, 255, 255);
            for (int i = 0; i < LED_STRIP_COUNT; i++) {
                led_rgb_t shifted = hsv_to_rgb((hue + i * 60) % 256, 255, 255);
                drv_led_strip_set_pixel(i, shifted);
            }
            drv_led_strip_refresh();
            hue = (hue + 4) % 256;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        break;
    }
        
    case LED_EFFECT_FLOW: {
        int offset = 0;
        while (!s_effect_stop) {
            drv_led_strip_clear();
            for (int i = 0; i < 2; i++) {
                int idx = (offset + i) % LED_STRIP_COUNT;
                drv_led_strip_set_pixel(idx, base_color);
            }
            drv_led_strip_refresh();
            offset = (offset + 1) % LED_STRIP_COUNT;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        break;
    }
        
    case LED_EFFECT_BLINK:
        while (!s_effect_stop) {
            drv_led_strip_set_all(base_color);
            drv_led_strip_refresh();
            vTaskDelay(pdMS_TO_TICKS(500));
            drv_led_strip_clear();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        break;
        
    default:
        break;
    }
    
    drv_led_strip_clear();
    s_effect_running = false;
    vTaskDelete(NULL);
}

/** 启动当前效果 */
static void start_effect(void)
{
    /* 停止当前效果 */
    if (s_effect_running) {
        s_effect_stop = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    /* 初始化 LED（如果未初始化） */
    drv_led_strip_init();
    drv_led_strip_set_brightness(s_current_brightness);
    
    /* 启动新效果任务 */
    xTaskCreate(led_effect_task, "led_effect", 2048, NULL, 5, NULL);
}

/* ========== UI 相关 ========== */

/** 获取当前页面行数 */
static int get_visible_rows(void)
{
    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_row_h;
    if (vis_rows < 1) vis_rows = 1;
    return vis_rows;
}

/** 重新构建可见行 */
static void led_rebuild_visible(void)
{
    if (!s_led_obj) return;
    
    lv_obj_clean(s_led_obj);
    
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_row_h = font_px + 2;
    if (s_row_h < 16) s_row_h = 16;
    
    int vis_rows = get_visible_rows();
    
    /* 总行数：效果列表 + 颜色列表 + 亮度 */
    int total_rows = LED_EFFECT_COUNT + 2 + COLOR_PRESET_COUNT;
    
    /* 确保选中项可见 */
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis_rows) s_scroll = s_sel - vis_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > total_rows - vis_rows) s_scroll = total_rows - vis_rows;
    if (s_scroll < 0) s_scroll = 0;
    
    int row_idx = 0;
    
    /* ---- 效果列表 ---- */
    for (int i = 0; i < LED_EFFECT_COUNT; i++) {
        if (row_idx < s_scroll || row_idx >= s_scroll + vis_rows) {
            row_idx++;
            continue;
        }
        int vis_i = row_idx - s_scroll;
        
        lv_obj_t *row = lv_obj_create(s_led_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, vis_i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        
        if (i == s_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 50);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
        lv_label_set_text(lbl, s_effect_names_zh[i]);
        
        /* 当前效果标记 */
        if (s_current_effect == i) {
            lv_obj_t *mark = lv_label_create(row);
            lv_obj_set_style_text_font(mark, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(mark, lv_color_hex(0x22C55E), 0);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -2, 0);
            lv_label_set_text(mark, LV_SYMBOL_OK);
        }
        
        row_idx++;
    }
    
    /* ---- 分隔线 ---- */
    if (row_idx >= s_scroll && row_idx < s_scroll + vis_rows) {
        lv_obj_t *sep = lv_label_create(s_led_obj);
        lv_obj_set_style_text_font(sep, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(sep, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_pos(sep, 2, (row_idx - s_scroll) * s_row_h);
        lv_label_set_text(sep, "─── 颜色 ───");
    }
    row_idx++;
    
    /* ---- 颜色列表 ---- */
    for (int i = 0; i < (int)COLOR_PRESET_COUNT; i++) {
        if (row_idx < s_scroll || row_idx >= s_scroll + vis_rows) {
            row_idx++;
            continue;
        }
        int vis_i = row_idx - s_scroll;
        
        lv_obj_t *row = lv_obj_create(s_led_obj);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, vis_i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        
        if (i == s_sel - (LED_EFFECT_COUNT + 1)) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        
        /* 颜色小方块 */
        lv_obj_t *color_box = lv_obj_create(row);
        lv_obj_remove_style_all(color_box);
        lv_obj_set_size(color_box, font_px - 2, font_px - 2);
        lv_obj_set_pos(color_box, 2, 1);
        lv_obj_set_style_bg_color(color_box, lv_color_hex(
            (s_color_presets[i].color.r << 16) |
            (s_color_presets[i].color.g << 8) |
            s_color_presets[i].color.b), 0);
        lv_obj_set_style_bg_opa(color_box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(color_box, 2, 0);
        
        /* 颜色名 */
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_x(lbl, font_px + 4);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, font_px + 4, 0);
        lv_label_set_text(lbl, s_color_presets[i].name);
        
        if (s_current_color == i) {
            lv_obj_t *mark = lv_label_create(row);
            lv_obj_set_style_text_font(mark, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(mark, lv_color_hex(0x22C55E), 0);
            lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -2, 0);
            lv_label_set_text(mark, LV_SYMBOL_OK);
        }
        
        row_idx++;
    }
    
    /* 更新信息标签 */
    if (s_info_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "亮度:%d  A:应用  B:返回",
                 s_current_brightness * 100 / 255);
        lv_label_set_text(s_info_label, buf);
    }
}

/* ========== 页面生命周期回调 ========== */

static void led_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "LED effect app init");
    
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    ui_statusbar_create(scr);
    ui_statusbar_set_title("灯效");
    
    s_row_h = ui_state_get()->font_size + 2;
    if (s_row_h < 16) s_row_h = 16;
    
    /* 列表容器 */
    s_led_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_led_obj);
    lv_obj_set_pos(s_led_obj, 0, ui_content_y());
    lv_obj_set_size(s_led_obj, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 12);
    lv_obj_clear_flag(s_led_obj, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 底部信息栏 */
    s_info_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_info_label, lv_font_cn_get(ui_state_get()->font_size), 0);
    lv_obj_set_style_text_color(s_info_label, lv_color_hex(colors->text_dim), 0);
    lv_obj_set_width(s_info_label, LCD_H_RES - 4);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(s_info_label, 2, LCD_V_RES - DOCK_H - 12);
    
    /* 底部导航栏 */
    ui_dock_create(scr, 1, 0);
    
    /* 初始化 LED 驱动 */
    drv_led_strip_init();
    s_current_brightness = drv_led_strip_get_brightness();
    
    s_sel = (int)s_current_effect;
    s_scroll = 0;
    led_rebuild_visible();
}

static void led_destroy(void)
{
    ESP_LOGI(TAG, "LED app destroy");
    s_led_obj = NULL;
    s_info_label = NULL;
}

static void led_activate(void)
{
    ESP_LOGI(TAG, "LED app activate");
    led_rebuild_visible();
}

static bool led_on_key(int key)
{
    /* 总行数 */
    int total_rows = LED_EFFECT_COUNT + 1 + COLOR_PRESET_COUNT;
    
    switch (key) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                led_rebuild_visible();
            }
            return true;
            
        case KEY_DOWN:
            if (s_sel < total_rows - 1) {
                s_sel++;
                led_rebuild_visible();
            }
            return true;
            
        case KEY_LEFT:
            /* 亮度减 */
            if (s_current_brightness > 20) {
                s_current_brightness -= 20;
                drv_led_strip_set_brightness(s_current_brightness);
                led_rebuild_visible();
            }
            return true;
            
        case KEY_RIGHT:
            /* 亮度加 */
            if (s_current_brightness < 240) {
                s_current_brightness += 20;
                drv_led_strip_set_brightness(s_current_brightness);
                led_rebuild_visible();
            }
            return true;
            
        case KEY_A: {
            /* 选择效果或颜色 */
            if (s_sel < LED_EFFECT_COUNT) {
                /* 选择效果 */
                s_current_effect = (led_effect_t)s_sel;
                start_effect();
                led_rebuild_visible();
            } else {
                int color_idx = s_sel - LED_EFFECT_COUNT - 1;
                if (color_idx >= 0 && color_idx < (int)COLOR_PRESET_COUNT) {
                    s_current_color = color_idx;
                    /* 如果当前有效果运行，重新应用 */
                    if (s_current_effect != LED_EFFECT_OFF) {
                        start_effect();
                    }
                    led_rebuild_visible();
                }
            }
            return true;
        }
            
        case KEY_B:
            if (ui_stack_depth() > 1) ui_stack_pop();
            return true;
            
        default:
            return true;
    }
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_led_callbacks = {
    .init = led_init,
    .activate = led_activate,
    .destroy = led_destroy,
    .on_key = led_on_key,
};