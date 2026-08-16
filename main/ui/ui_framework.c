/**
 * @file ui_framework.c
 * @brief UI框架核心实现（v59 重构版）
 * 
 * 架构设计：
 * - 统一栈数组支持 v1 和 v2 回调
 * - 页面生命周期状态机（8个状态）
 * - v1 兼容层自动映射旧回调到新生命周期
 */

#include "ui_framework.h"
#include "app/app_manager.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
// FreeType 字体支持（统一中文字体入口）
#include "fonts/lv_freetype_font.h"

static const char *TAG = "UI_FW";

/* ========== 统一栈结构（支持 v1/v2） ========== */
#define MAX_STACK_DEPTH 8

typedef struct {
    page_type_t type;
    const page_callbacks_v2_t *callbacks_v2;  // v2 回调
    const page_callbacks_t *callbacks_v1;     // v1 回调（兼容）
    void *data;
    page_state_t state;
    bool cached;
} stack_entry_v2_t;

static stack_entry_v2_t s_page_stack_v2[MAX_STACK_DEPTH];
static int s_stack_top = -1;

/* ========== Stash 全局缓冲区 ========== */
static page_stash_t s_stash_global = {
    .valid = false,
    .size = 0,
};

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
    .brand_label = NULL,
    .time_label = NULL,
    .bat_label = NULL,
    .theme = THEME_DARK,
    .brightness = 75,
    .sound_on = true,
    .wifi_on = true,
    .layout = 0,
};

/* ========== v1 兼容辅助函数 ========== */

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

/* ========== 页面栈管理 API ========== */

void ui_stack_init(void)
{
    s_stack_top = -1;
    memset(s_page_stack_v2, 0, sizeof(s_page_stack_v2));
    ESP_LOGI(TAG, "Page stack initialized");
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

/* 旧版 push（v1 兼容） */
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

/* pop 支持 v2 生命周期 */
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
        /*
         * v1 页面兼容的生命周期修复：
         *
         * push 时对 v1 页面执行的是 init + activate（见 ui_stack_push）。
         * 但 pop 回上一个 v1 页面时，只调用了 activate（DID_APPEAR），
         * 没有重新执行 init（LOAD）。
         *
         * 问题后果：当上层应用在 activate 里执行 lv_obj_clean(scr) 清空屏幕后，
         * 底层页面（如桌面）自己创建的 LVGL 对象已被销毁，成为悬空指针。
         * 若该页面在 on_key 中继续访问这些对象（例如 desktop_page_on_key
         * 里的 s_app_cells[]），就会访问已释放内存，引发 LoadProhibited 崩溃，
         * 且屏幕上无法重建该页面的 UI（表现为"没有返回到桌面"）。
         *
         * 修复：pop 回 v1 页面时，恢复与 push 对称的生命周期——
         * 先重新执行 init（重建 LVGL 对象），再 activate。
         * v2 页面有 cached/should_cache 机制，走原有生命周期即可。
         */
        if (prev->callbacks_v1) {
            if (prev->callbacks_v1->init) {
                prev->callbacks_v1->init(prev->data);
            }
        }
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

/* 旧版回调获取（兼容） */
const page_callbacks_t* ui_stack_current_callbacks(void)
{
    if (s_stack_top < 0) return NULL;
    return s_page_stack_v2[s_stack_top].callbacks_v1;
}

/* 获取当前页面类型 */
page_type_t ui_stack_current(void)
{
    if (s_stack_top < 0) return PAGE_DESKTOP;
    return s_page_stack_v2[s_stack_top].type;
}

/* 栈深度 */
int ui_stack_depth(void)
{
    return s_stack_top + 1;
}

/* 清空栈 */
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

/* ========== 主题系统 ========== */

static theme_type_t s_current_theme = THEME_DARK;

void ui_theme_set(theme_type_t theme)
{
    if (theme >= THEME_MAX) return;
    s_current_theme = theme;
    s_ui_state.theme = theme;
    ESP_LOGI(TAG, "Theme set to %s", theme == THEME_DARK ? "DARK" : "LIGHT");
}

theme_type_t ui_theme_get(void)
{
    return s_current_theme;
}

const theme_colors_t* ui_theme_colors(void)
{
    return &s_themes[s_current_theme];
}

/* ========== UI全局状态 ========== */

ui_state_t* ui_state_get(void)
{
    return &s_ui_state;
}

/* ========== 通用UI组件 ========== */

lv_obj_t* ui_statusbar_create(lv_obj_t *parent)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_H_RES, STATUS_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    
    // 品牌名/应用名（英文，使用默认字体避免乱码）
    lv_obj_t *brand = lv_label_create(bar);
    // 如果当前有应用在运行，显示应用名；否则显示品牌名
    const char *app_name = app_manager_get_current_name();
    if (app_name) {
        lv_label_set_text(brand, app_name);
    } else {
        lv_label_set_text(brand, "XiaoMiaoOS");
    }
    lv_obj_set_style_text_color(brand, lv_color_hex(colors->text), 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 4, 0);
    s_ui_state.brand_label = brand;
    
    // 时间标签
    lv_obj_t *time = lv_label_create(bar);
    lv_label_set_text(time, "00:00");
    lv_obj_set_style_text_color(time, lv_color_hex(colors->text), 0);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, 0);
    s_ui_state.time_label = time;
    
    // 电池标签
    lv_obj_t *bat = lv_label_create(bar);
    lv_label_set_text(bat, "100%");
    lv_obj_set_style_text_color(bat, lv_color_hex(colors->text), 0);
    lv_obj_align(bat, LV_ALIGN_RIGHT_MID, -4, 0);
    s_ui_state.bat_label = bat;
    
    s_ui_state.statusbar = bar;
    return bar;
}

void ui_statusbar_update_time(void)
{
    if (!s_ui_state.time_label) return;
    
    time_t now;
    struct tm *tm_info;
    time(&now);
    tm_info = localtime(&now);
    
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
    lv_label_set_text(s_ui_state.time_label, buf);
}

void ui_statusbar_update_battery(void)
{
    // 由主循环调用，此处留空
}

void ui_statusbar_set_title(const char *title)
{
    if (!s_ui_state.brand_label) return;
    if (title && title[0] != '\0') {
        lv_label_set_text(s_ui_state.brand_label, title);
    } else {
        lv_label_set_text(s_ui_state.brand_label, "XiaoMiaoOS");
    }
}

lv_obj_t* ui_dock_create(lv_obj_t *parent, int total_pages, int active_idx)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *dock = lv_obj_create(parent);
    lv_obj_remove_style_all(dock);
    lv_obj_set_pos(dock, 0, LCD_V_RES - DOCK_H);
    lv_obj_set_size(dock, LCD_H_RES, DOCK_H);
    lv_obj_set_style_bg_color(dock, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    
    // 页面指示器（圆点）
    if (total_pages > 1) {
        int dot_spacing = LCD_H_RES / (total_pages + 1);
        for (int i = 0; i < total_pages; i++) {
            lv_obj_t *dot = lv_obj_create(dock);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_set_pos(dot, dot_spacing * (i + 1) - 2, 2);
            lv_obj_set_style_radius(dot, 2, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(i == active_idx ? colors->text : colors->text_dim), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
    }
    
    return dock;
}

lv_obj_t* ui_titlebar_create(lv_obj_t *parent, lv_coord_t y, const char *text)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_size(bar, LCD_H_RES, 14);
    lv_obj_set_style_bg_color(bar, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
    // 标题为中文，使用统一中文字体（优先FreeType，回退内置）
    lv_obj_set_style_text_font(lbl, lv_font_cn_14(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    
    return bar;
}

void ui_desktop_cell_set_selected(lv_obj_t *cell, bool selected)
{
    const theme_colors_t *colors = ui_theme_colors();
    if (selected) {
        lv_obj_set_style_bg_color(cell, lv_color_hex(colors->sel_bg), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    }
}
