/**
 * @file ui_framework.c
 * @brief UI框架核心实现
 */

#include "ui_framework.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "UI_FW";

/* ========== 页面栈实现 ========== */
#define MAX_STACK_DEPTH 8

typedef struct {
    page_type_t type;
    const page_callbacks_t *callbacks;
    void *data;
    bool active;
} stack_entry_t;

static stack_entry_t s_page_stack[MAX_STACK_DEPTH];
static int s_stack_top = -1;

/* ========== 主题定义 ========== */
static const theme_colors_t s_themes[THEME_MAX] = {
    [THEME_DARK] = {
        .bg = 0x0C0D10,
        .text = 0xE8E8EC,
        .text_dim = 0x9AA0AC,
        .header_bg = 0x16181E,
        .border = 0x222222,
        .sel_bg = 0x2A3A5A,
        .sel_border = 0xFFFFFF,
    },
    [THEME_LIGHT] = {
        .bg = 0xF0F0F0,
        .text = 0x1A1A1A,
        .text_dim = 0x666666,
        .header_bg = 0xD8D8D8,
        .border = 0xCCCCCC,
        .sel_bg = 0xD0E0FF,
        .sel_border = 0x000000,
    },
};

/* ========== UI全局状态 ========== */
static ui_state_t s_ui_state = {
    .statusbar = NULL,
    .time_label = NULL,
    .bat_label = NULL,
    .theme = THEME_DARK,
    .brightness = 75,
    .sound_on = true,
    .wifi_on = true,
    .layout = 0,
};

/* ========== 页面栈管理 ========== */

void ui_stack_init(void)
{
    s_stack_top = -1;
    memset(s_page_stack, 0, sizeof(s_page_stack));
    ESP_LOGI(TAG, "Page stack initialized");
}

void ui_stack_push(page_type_t type, const page_callbacks_t *callbacks, void *data)
{
    if (s_stack_top >= MAX_STACK_DEPTH - 1) {
        ESP_LOGE(TAG, "Page stack overflow!");
        return;
    }
    
    // 失活当前页面
    if (s_stack_top >= 0 && s_page_stack[s_stack_top].active) {
        if (s_page_stack[s_stack_top].callbacks && 
            s_page_stack[s_stack_top].callbacks->deactivate) {
            s_page_stack[s_stack_top].callbacks->deactivate();
        }
        s_page_stack[s_stack_top].active = false;
    }
    
    // 推入新页面
    s_stack_top++;
    s_page_stack[s_stack_top].type = type;
    s_page_stack[s_stack_top].callbacks = callbacks;
    s_page_stack[s_stack_top].data = data;
    s_page_stack[s_stack_top].active = true;
    
    // 初始化并激活新页面
    if (callbacks && callbacks->init) {
        callbacks->init(data);
    }
    if (callbacks && callbacks->activate) {
        callbacks->activate();
    }
    
    ESP_LOGI(TAG, "Push page type=%d, stack depth=%d", type, s_stack_top + 1);
}

bool ui_stack_pop(void)
{
    if (s_stack_top < 0) {
        ESP_LOGW(TAG, "Page stack is empty, cannot pop");
        return false;
    }
    
    // 销毁当前页面
    if (s_page_stack[s_stack_top].callbacks) {
        if (s_page_stack[s_stack_top].callbacks->deactivate) {
            s_page_stack[s_stack_top].callbacks->deactivate();
        }
        if (s_page_stack[s_stack_top].callbacks->destroy) {
            s_page_stack[s_stack_top].callbacks->destroy();
        }
    }
    s_page_stack[s_stack_top].active = false;
    s_stack_top--;
    
    // 激活上一页面
    if (s_stack_top >= 0) {
        s_page_stack[s_stack_top].active = true;
        if (s_page_stack[s_stack_top].callbacks && 
            s_page_stack[s_stack_top].callbacks->activate) {
            s_page_stack[s_stack_top].callbacks->activate();
        }
        ESP_LOGI(TAG, "Pop page, now type=%d, depth=%d", 
                 s_page_stack[s_stack_top].type, s_stack_top + 1);
    } else {
        ESP_LOGI(TAG, "Page stack is now empty");
    }
    
    return true;
}

page_type_t ui_stack_current(void)
{
    if (s_stack_top < 0) {
        return PAGE_DESKTOP;  // 默认返回桌面
    }
    return s_page_stack[s_stack_top].type;
}

int ui_stack_depth(void)
{
    return s_stack_top + 1;
}

void ui_stack_clear(void)
{
    while (s_stack_top > 0) {
        ui_stack_pop();
    }
}

/* ========== 主题系统 ========== */

void ui_theme_set(theme_type_t theme)
{
    if (theme >= THEME_MAX) {
        ESP_LOGW(TAG, "Invalid theme: %d", theme);
        return;
    }
    s_ui_state.theme = theme;
    ESP_LOGI(TAG, "Theme set to %s", theme == THEME_DARK ? "Dark" : "Light");
}

theme_type_t ui_theme_get(void)
{
    return s_ui_state.theme;
}

const theme_colors_t* ui_theme_colors(void)
{
    return &s_themes[s_ui_state.theme];
}

/* ========== 通用UI组件 ========== */

#define STATUS_H    12
#define DOCK_H      10

lv_obj_t* ui_statusbar_create(lv_obj_t *parent)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_size(sb, LCD_H_RES, STATUS_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_90, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_set_style_pad_left(sb, 4, 0);
    lv_obj_set_style_pad_right(sb, 4, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 品牌名
    lv_obj_t *brand = lv_label_create(sb);
    lv_label_set_text(brand, "XIAOMIAO");
    lv_obj_set_style_text_color(brand, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_12, 0);
    
    // 右侧容器
    lv_obj_t *rc = lv_obj_create(sb);
    lv_obj_remove_style_all(rc);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_END, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rc, 4, 0);
    lv_obj_clear_flag(rc, LV_OBJ_FLAG_SCROLLABLE);
    
    // 时间标签
    s_ui_state.time_label = lv_label_create(rc);
    lv_label_set_text(s_ui_state.time_label, "12:00");
    lv_obj_set_style_text_color(s_ui_state.time_label, 
                                 lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(s_ui_state.time_label, 
                                &lv_font_montserrat_12, 0);
    
    // 电池标签
    s_ui_state.bat_label = lv_label_create(rc);
    lv_label_set_text(s_ui_state.bat_label, "85%");
    lv_obj_set_style_text_color(s_ui_state.bat_label, 
                                 lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_text_font(s_ui_state.bat_label, 
                                &lv_font_montserrat_12, 0);
    
    s_ui_state.statusbar = sb;
    return sb;
}

void ui_statusbar_update_time(void)
{
    if (!s_ui_state.time_label || !lv_obj_is_valid(s_ui_state.time_label)) {
        return;
    }
    
    time_t nowt;
    time(&nowt);
    struct tm *tm_info = localtime(&nowt);
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", 
             tm_info->tm_hour, tm_info->tm_min);
    lv_label_set_text(s_ui_state.time_label, tbuf);
}

void ui_statusbar_update_battery(void)
{
    // 电池更新由外部驱动调用，这里只更新显示
    // 具体实现在 drv_battery.c 中
}

lv_obj_t* ui_dock_create(lv_obj_t *parent, int total_pages, int active_idx)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_set_pos(dock, 0, LCD_V_RES - DOCK_H);
    lv_obj_set_size(dock, LCD_H_RES, DOCK_H);
    lv_obj_set_style_bg_color(dock, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_90, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_border_side(dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(colors->border), 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_CENTER, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dock, 8, 0);
    
    // 页面指示器圆点
    int dot_count = (total_pages > 0) ? total_pages : 1;
    for (int i = 0; i < dot_count; i++) {
        lv_obj_t *dot = lv_obj_create(dock);
        lv_obj_set_size(dot, 3, 3);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        
        if (i == active_idx) {
            lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(colors->border), 0);
        }
    }
    
    return dock;
}

ui_state_t* ui_state_get(void)
{
    return &s_ui_state;
}