/**
 * @file ui_framework.c
 * @brief UI框架核心实现
 */

#include "ui_framework.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
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
// 小喵OS 特色配色：黄色主题（模拟器风格）
// 黄:#F6D34A  黑:#1B1713  棕:#5C4220  红:#E64B3C  奶:#FFF3B0  绿:#2DD466
static const theme_colors_t s_themes[THEME_MAX] = {
    [THEME_DARK] = {
        .bg = 0xF6D34A,         // 黄色背景
        .text = 0x1B1713,       // 黑色文字
        .text_dim = 0x5C4220,   // 棕色次要文字
        .header_bg = 0x5C4220,  // 棕色状态栏/标题栏
        .border = 0x5C4220,     // 棕色边框
        .sel_bg = 0x5C4220,     // 棕色选中背景
        .sel_border = 0x5C4220, // 棕色选中边框
    },
    [THEME_LIGHT] = {
        .bg = 0xF6D34A,         // 黄色背景（同色，不区分）
        .text = 0x1B1713,
        .text_dim = 0x5C4220,
        .header_bg = 0x5C4220,
        .border = 0x5C4220,
        .sel_bg = 0x5C4220,
        .sel_border = 0x5C4220,
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

const page_callbacks_t* ui_stack_current_callbacks(void)
{
    if (s_stack_top < 0) {
        return NULL;
    }
    return s_page_stack[s_stack_top].callbacks;
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

// STATUS_H 和 DOCK_H 定义在 ui_framework.h 中

lv_obj_t* ui_statusbar_create(lv_obj_t *parent)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *sb = lv_obj_create(parent);
    lv_obj_set_pos(sb, 0, 0);
    lv_obj_set_size(sb, LCD_H_RES, STATUS_H);
    lv_obj_set_style_bg_color(sb, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(sb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sb, 0, 0);
    lv_obj_set_style_pad_all(sb, 0, 0);
    lv_obj_set_style_pad_left(sb, 3, 0);
    lv_obj_set_style_pad_right(sb, 3, 0);
    lv_obj_clear_flag(sb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_SPACE_BETWEEN, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // 时间（左）
    s_ui_state.time_label = lv_label_create(sb);
    lv_label_set_text(s_ui_state.time_label, "12:00");
    lv_obj_set_style_text_color(s_ui_state.time_label, 
                                 lv_color_hex(0xFFF3B0), 0);  // 奶油色
    lv_obj_set_style_text_font(s_ui_state.time_label, 
                                &lv_font_montserrat_8, 0);
    
    // 品牌名（中）- 使用中文"CJK"字体
    lv_obj_t *brand = lv_label_create(sb);
    lv_label_set_text(brand, "小喵OS");
    lv_obj_set_style_text_color(brand, lv_color_hex(0xFFF3B0), 0);
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(brand, &lv_font_xiaomiao_cn_14, 0);
    
    // 右侧容器（电池图标）
    lv_obj_t *rc = lv_obj_create(sb);
    lv_obj_remove_style_all(rc);
    lv_obj_set_flex_flow(rc, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_END, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rc, 2, 0);
    lv_obj_clear_flag(rc, LV_OBJ_FLAG_SCROLLABLE);
    
    // 电池百分比
    s_ui_state.bat_label = lv_label_create(rc);
    lv_label_set_text(s_ui_state.bat_label, "85%");
    lv_obj_set_style_text_color(s_ui_state.bat_label, 
                                 lv_color_hex(0x2DD466), 0);  // 绿色
    lv_obj_set_style_text_font(s_ui_state.bat_label, 
                                &lv_font_montserrat_8, 0);
    
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
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_CENTER, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dock, 3, 0);
    
    // 页面指示器圆点（模拟器样式）
    // .dot{width:3px;height:3px;border-radius:50%;background:var(--brown);opacity:.35}
    // .dot.on{opacity:1}
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
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x5C4220), 0); // brown
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);            // opacity:1
        } else {
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x5C4220), 0); // brown
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);               // opacity:.35 (use .30 as closest)
        }
    }
    
    return dock;
}

ui_state_t* ui_state_get(void)
{
    return &s_ui_state;
}

/* ========== 通用标题栏 ========== */
lv_obj_t* ui_titlebar_create(lv_obj_t *parent, lv_coord_t y, const char *text)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *tb = lv_obj_create(parent);
    lv_obj_set_pos(tb, 0, y);
    lv_obj_set_size(tb, LCD_H_RES, 12);
    lv_obj_set_style_bg_color(tb, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tb, 0, 0);
    lv_obj_set_style_pad_all(tb, 0, 0);
    lv_obj_set_style_pad_left(tb, 4, 0);
    lv_obj_clear_flag(tb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_START, 
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t *lbl = lv_label_create(tb);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFF3B0), 0);  // 奶油色
    // 标题栏使用 CJK 14px 字体以支持中文显示
    // 在 sdkconfig 中启用 CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
    
    return tb;
}
/* ========== v59 重构：页面生命周期管理（借鉴 X-TRACK） ========== */

/* 页面状态枚举已在头文件中定义 */

/* Stash 全局缓冲区 */
static page_stash_t s_stash_global = {0};

/* v2 页面栈条目 */
typedef struct {
    page_type_t type;
    const page_callbacks_v2_t *callbacks_v2;
    const page_callbacks_t *callbacks_v1;
    void *data;
    page_state_t state;
    bool cached;
} stack_entry_v2_t;

/* 使用统一的栈数组（兼容 v1 和 v2） */
static stack_entry_v2_t s_page_stack_v2[MAX_STACK_DEPTH];

/* v1 兼容辅助函数 */
static void v1_compat_on_load(void *data)
{
    if (s_stack_top >= 0 && s_page_stack_v2[s_stack_top].callbacks_v1) {
        if (s_page_stack_v2[s_stack_top].callbacks_v1->init) {
            s_page_stack_v2[s_stack_top].callbacks_v1->init(data);
        }
    }
}

static void v1_compat_on_did_appear(void)
{
    if (s_stack_top >= 0 && s_page_stack_v2[s_stack_top].callbacks_v1) {
        if (s_page_stack_v2[s_stack_top].callbacks_v1->activate) {
            s_page_stack_v2[s_stack_top].callbacks_v1->activate();
        }
    }
}

static void v1_compat_on_will_disappear(void)
{
    if (s_stack_top >= 0 && s_page_stack_v2[s_stack_top].callbacks_v1) {
        if (s_page_stack_v2[s_stack_top].callbacks_v1->deactivate) {
            s_page_stack_v2[s_stack_top].callbacks_v1->deactivate();
        }
    }
}

static void v1_compat_on_unload(void)
{
    if (s_stack_top >= 0 && s_page_stack_v2[s_stack_top].callbacks_v1) {
        if (s_page_stack_v2[s_stack_top].callbacks_v1->destroy) {
            s_page_stack_v2[s_stack_top].callbacks_v1->destroy();
        }
    }
}

/* 执行生命周期回调 */
static void execute_lifecycle(page_state_t to, stack_entry_v2_t *entry)
{
    if (!entry) return;
    
    const page_callbacks_v2_t *cb = entry->callbacks_v2;
    
    if (!cb) {
        /* v1 兼容模式 */
        switch (to) {
            case PAGE_STATE_LOAD:           v1_compat_on_load(entry->data); break;
            case PAGE_STATE_DID_APPEAR:     v1_compat_on_did_appear(); break;
            case PAGE_STATE_WILL_DISAPPEAR: v1_compat_on_will_disappear(); break;
            case PAGE_STATE_UNLOAD:         v1_compat_on_unload(); break;
            default: break;
        }
        return;
    }
    
    /* v2 回调 */
    switch (to) {
        case PAGE_STATE_LOAD:
            if (cb->on_load) cb->on_load(entry->data);
            break;
        case PAGE_STATE_WILL_APPEAR:
            if (cb->on_will_appear) cb->on_will_appear();
            break;
        case PAGE_STATE_DID_APPEAR:
            if (cb->on_did_appear) cb->on_did_appear();
            break;
        case PAGE_STATE_WILL_DISAPPEAR:
            if (cb->on_will_disappear) cb->on_will_disappear();
            break;
        case PAGE_STATE_DID_DISAPPEAR:
            if (cb->on_did_disappear) cb->on_did_disappear();
            break;
        case PAGE_STATE_UNLOAD:
            if (cb->on_unload) cb->on_unload();
            break;
        default: break;
    }
}

/* v2 版本 push */
void ui_stack_push_v2(page_type_t type, const page_callbacks_v2_t *callbacks, void *data)
{
    if (s_stack_top >= MAX_STACK_DEPTH - 1) {
        ESP_LOGE(TAG, "Page stack overflow!");
        return;
    }
    
    /* 失活当前页面 */
    if (s_stack_top >= 0) {
        stack_entry_v2_t *current = &s_page_stack_v2[s_stack_top];
        execute_lifecycle(PAGE_STATE_WILL_DISAPPEAR, current);
        execute_lifecycle(PAGE_STATE_DID_DISAPPEAR, current);
    }
    
    /* 推入新页面 */
    s_stack_top++;
    stack_entry_v2_t *new_entry = &s_page_stack_v2[s_stack_top];
    new_entry->type = type;
    new_entry->callbacks_v2 = callbacks;
    new_entry->callbacks_v1 = NULL;
    new_entry->data = data;
    new_entry->state = PAGE_STATE_IDLE;
    new_entry->cached = false;
    
    if (callbacks && callbacks->should_cache && callbacks->should_cache()) {
        new_entry->cached = true;
        ESP_LOGI(TAG, "Page %d will be cached", type);
    }
    
    execute_lifecycle(PAGE_STATE_LOAD, new_entry);
    execute_lifecycle(PAGE_STATE_WILL_APPEAR, new_entry);
    execute_lifecycle(PAGE_STATE_DID_APPEAR, new_entry);
    new_entry->state = PAGE_STATE_ACTIVITY;
    
    ESP_LOGI(TAG, "Push page v2 type=%d, depth=%d", type, s_stack_top + 1);
}

/* 重写旧版 push 以使用统一栈 */
void ui_stack_push(page_type_t type, const page_callbacks_t *callbacks, void *data)
{
    if (s_stack_top >= MAX_STACK_DEPTH - 1) {
        ESP_LOGE(TAG, "Page stack overflow!");
        return;
    }
    
    if (s_stack_top >= 0 && s_page_stack_v2[s_stack_top].callbacks_v1) {
        if (s_page_stack_v2[s_stack_top].callbacks_v1->deactivate) {
            s_page_stack_v2[s_stack_top].callbacks_v1->deactivate();
        }
    }
    
    s_stack_top++;
    s_page_stack_v2[s_stack_top].type = type;
    s_page_stack_v2[s_stack_top].callbacks_v2 = NULL;
    s_page_stack_v2[s_stack_top].callbacks_v1 = callbacks;
    s_page_stack_v2[s_stack_top].data = data;
    s_page_stack_v2[s_stack_top].state = PAGE_STATE_ACTIVITY;
    s_page_stack_v2[s_stack_top].cached = false;
    
    if (callbacks && callbacks->init) callbacks->init(data);
    if (callbacks && callbacks->activate) callbacks->activate();
    
    ESP_LOGI(TAG, "Push page v1 type=%d, depth=%d", type, s_stack_top + 1);
}

/* 重写 pop 以支持 v2 生命周期 */
bool ui_stack_pop(void)
{
    if (s_stack_top < 0) {
        ESP_LOGW(TAG, "Page stack empty");
        return false;
    }
    
    stack_entry_v2_t *current = &s_page_stack_v2[s_stack_top];
    
    if (!current->cached) {
        execute_lifecycle(PAGE_STATE_WILL_DISAPPEAR, current);
        execute_lifecycle(PAGE_STATE_DID_DISAPPEAR, current);
        execute_lifecycle(PAGE_STATE_UNLOAD, current);
    } else {
        ESP_LOGI(TAG, "Page %d cached", current->type);
    }
    
    s_stack_top--;
    
    if (s_stack_top >= 0) {
        stack_entry_v2_t *prev = &s_page_stack_v2[s_stack_top];
        execute_lifecycle(PAGE_STATE_WILL_APPEAR, prev);
        execute_lifecycle(PAGE_STATE_DID_APPEAR, prev);
        prev->state = PAGE_STATE_ACTIVITY;
        ESP_LOGI(TAG, "Pop, now type=%d, depth=%d", prev->type, s_stack_top + 1);
    }
    
    return true;
}

/* BackHome 功能 */
void ui_stack_back_home(void)
{
    ESP_LOGI(TAG, "Back to home");
    while (s_stack_top > 0) {
        ui_stack_pop();
    }
}

/* v2 回调获取 */
const page_callbacks_v2_t* ui_stack_current_callbacks_v2(void)
{
    if (s_stack_top < 0) return NULL;
    return s_page_stack_v2[s_stack_top].callbacks_v2;
}

/* 重写旧版回调获取 */
const page_callbacks_t* ui_stack_current_callbacks(void)
{
    if (s_stack_top < 0) return NULL;
    return s_page_stack_v2[s_stack_top].callbacks_v1;
}

/* 重写栈深度 */
int ui_stack_depth(void)
{
    return s_stack_top + 1;
}

/* 重写清空栈 */
void ui_stack_clear(void)
{
    while (s_stack_top > 0) {
        ui_stack_pop();
    }
}

/* Stash 数据传递 */
void ui_stash_set(const page_stash_t *stash)
{
    if (!stash || !stash->valid) return;
    memcpy(s_stash_global.data, stash->data, stash->size);
    s_stash_global.size = stash->size;
    s_stash_global.valid = true;
    ESP_LOGI(TAG, "Stash set: %lu bytes", stash->size);
}

page_stash_t* ui_stash_pop(void)
{
    if (!s_stash_global.valid) return NULL;
    s_stash_global.valid = false;
    ESP_LOGI(TAG, "Stash popped: %lu bytes", s_stash_global.size);
    return &s_stash_global;
}
}