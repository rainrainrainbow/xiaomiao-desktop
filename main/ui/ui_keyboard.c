/**
 * @file ui_keyboard.c
 * @brief 虚拟键盘组件实现 - 紧凑全键盘输入（适配160x128小屏+6键导航）
 */

#include "ui_keyboard.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UI_KB";

/* ========== 字符集定义 ========== */
static const char *kb_chars_lower[] = {
    "q", "w", "e", "r", "t", "y", "u",
    "a", "s", "d", "f", "g", "h", "j",
    "z", "x", "c", "v", "b", "n", "m",
    "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "0", " ", ".", ",", "OK"
};

static const char *kb_chars_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U",
    "A", "S", "D", "F", "G", "H", "J",
    "Z", "X", "C", "V", "B", "N", "M",
    "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "0", " ", ".", ",", "OK"
};

static const char *kb_chars_number[] = {
    "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "0", "-", "_", "=", "+",
    "!", "@", "#", "$", "%", "^", "&",
    "*", "(", ")", "[", "]", "{", "}",
    "|", "\\", "/", ":", ";", "\"", "OK"
};

static const char *kb_chars_symbol[] = {
    "~", "`", "<", ">", "·", "•", "©",
    "®", "™", "€", "£", "¥", "¢", "§",
    "±", "×", "÷", "≈", "≠", "≤", "≥",
    "∞", "∑", "∏", "√", "∫", "∂", "∆",
    "←", "→", "↑", "↓", "↔", "↕", "OK"
};

/* ========== 布局常量 ========== */
#define KB_COLS          7      /* 每行字符数 */
#define KB_ROWS          5      /* 行数 */
#define KB_CELL_W        22     /* 单元格宽度 */
#define KB_CELL_H        18     /* 单元格高度 */
#define KB_INPUT_H       24     /* 输入框高度 */
#define KB_ACTION_H      20     /* 操作栏高度 */
#define KB_PADDING       4      /* 边距 */

/* ========== 键盘状态 ========== */
static bool s_kb_visible = false;
static lv_obj_t *s_kb_container = NULL;
static lv_obj_t *s_kb_input = NULL;
static lv_obj_t *s_kb_grid[KB_ROWS][KB_COLS] = {{NULL}};
static lv_obj_t *s_kb_action_bar = NULL;
static lv_obj_t *s_kb_mode_label = NULL;
static lv_obj_t *s_kb_cursor = NULL;

static kb_config_t s_kb_config;
static char s_kb_text[128] = {0};
static int s_kb_text_len = 0;
static kb_mode_t s_kb_mode = KB_MODE_LOWER;
static int s_kb_row = 0;
static int s_kb_col = 0;

/* ========== 辅助函数 ========== */
static const char* kb_get_char(int row, int col)
{
    int idx = row * KB_COLS + col;
    if (idx < 0 || idx >= KB_ROWS * KB_COLS) return NULL;
    
    switch (s_kb_mode) {
    case KB_MODE_LOWER:  return kb_chars_lower[idx];
    case KB_MODE_UPPER:  return kb_chars_upper[idx];
    case KB_MODE_NUMBER: return kb_chars_number[idx];
    case KB_MODE_SYMBOL: return kb_chars_symbol[idx];
    default: return NULL;
    }
}

static void kb_refresh_grid(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_obj_t *cell = s_kb_grid[r][c];
            if (!cell) continue;
            
            const char *ch = kb_get_char(r, c);
            lv_obj_t *lbl = (lv_obj_t*)lv_obj_get_child(cell, 0);
            if (lbl && ch) {
                lv_label_set_text(lbl, ch);
            }
            
            /* 高亮选中单元格 */
            if (r == s_kb_row && c == s_kb_col) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(colors->sel_bg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_hex(colors->bg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            }
        }
    }
}

static void kb_refresh_input(void)
{
    if (!s_kb_input) return;
    
    if (s_kb_text_len == 0) {
        lv_label_set_text(s_kb_input, s_kb_config.placeholder ? s_kb_config.placeholder : "");
        const theme_colors_t *colors = ui_theme_colors();
        lv_obj_set_style_text_color(s_kb_input, lv_color_hex(colors->text_dim), 0);
    } else {
        if (s_kb_config.password_mode) {
            char masked[128];
            int len = s_kb_text_len < 64 ? s_kb_text_len : 64;
            memset(masked, '*', len);
            masked[len] = '\0';
            lv_label_set_text(s_kb_input, masked);
        } else {
            lv_label_set_text(s_kb_input, s_kb_text);
        }
        const theme_colors_t *colors = ui_theme_colors();
        lv_obj_set_style_text_color(s_kb_input, lv_color_hex(colors->text), 0);
    }
}

static void kb_refresh_mode(void)
{
    if (!s_kb_mode_label) return;
    
    const char *mode_text = "";
    switch (s_kb_mode) {
    case KB_MODE_LOWER:  mode_text = "abc"; break;
    case KB_MODE_UPPER:  mode_text = "ABC"; break;
    case KB_MODE_NUMBER: mode_text = "123"; break;
    case KB_MODE_SYMBOL: mode_text = "#$%"; break;
    default: break;
    }
    lv_label_set_text(s_kb_mode_label, mode_text);
}

static void kb_append_char(const char *ch)
{
    if (!ch) return;
    
    int ch_len = strlen(ch);
    int max_len = s_kb_config.max_length > 0 ? s_kb_config.max_length : 64;
    
    if (s_kb_text_len + ch_len >= max_len) {
        ESP_LOGW(TAG, "Input too long (max=%d)", max_len);
        return;
    }
    
    strcpy(s_kb_text + s_kb_text_len, ch);
    s_kb_text_len += ch_len;
    kb_refresh_input();
}

static void kb_backspace(void)
{
    if (s_kb_text_len > 0) {
        s_kb_text[--s_kb_text_len] = '\0';
        kb_refresh_input();
    }
}

static void kb_confirm(void)
{
    if (s_kb_config.on_confirm) {
        s_kb_config.on_confirm(s_kb_text, s_kb_config.user_data);
    }
    ui_keyboard_hide();
}

static void kb_cancel(void)
{
    if (s_kb_config.on_cancel) {
        s_kb_config.on_cancel(s_kb_config.user_data);
    }
    ui_keyboard_hide();
}

/* ========== 公共接口 ========== */
void ui_keyboard_show(const kb_config_t *config)
{
    if (s_kb_visible) {
        ESP_LOGW(TAG, "Keyboard already visible");
        return;
    }
    
    if (!config) {
        ESP_LOGE(TAG, "Config is NULL");
        return;
    }
    
    s_kb_config = *config;
    s_kb_text[0] = '\0';
    s_kb_text_len = 0;
    s_kb_mode = KB_MODE_LOWER;
    s_kb_row = 0;
    s_kb_col = 0;
    
    const theme_colors_t *colors = ui_theme_colors();
    
    /* 创建全屏容器 */
    s_kb_container = lv_obj_create(lv_screen_active());
    if (!s_kb_container) { ESP_LOGE(TAG, "kb_container failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); return; }
    lv_obj_remove_style_all(s_kb_container);
    lv_obj_set_size(s_kb_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_kb_container, 0, 0);
    lv_obj_set_style_bg_color(s_kb_container, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(s_kb_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_SCROLLABLE);
    
    int y = KB_PADDING;
    
    /* 标题 */
    if (config->title) {
        lv_obj_t *title = lv_label_create(s_kb_container);
        if (title) {
        lv_label_set_text(title, config->title);
        lv_obj_set_style_text_color(title, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(title, lv_font_cn_get(14), 0);
        lv_obj_set_pos(title, KB_PADDING, y);
        y += 16;
        }
    }
    
    /* 输入框 */
    s_kb_input = lv_label_create(s_kb_container);
    if (!s_kb_input) { ESP_LOGE(TAG, "kb_input failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); return; }
    lv_obj_set_size(s_kb_input, LCD_H_RES - KB_PADDING * 2, KB_INPUT_H);
    lv_obj_set_pos(s_kb_input, KB_PADDING, y);
    lv_obj_set_style_bg_color(s_kb_input, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(s_kb_input, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_kb_input, 4, 0);
    lv_obj_set_style_pad_left(s_kb_input, 6, 0);
    lv_obj_set_style_pad_right(s_kb_input, 6, 0);
    lv_label_set_long_mode(s_kb_input, LV_LABEL_LONG_SCROLL_CIRCULAR);
    kb_refresh_input();
    y += KB_INPUT_H + 2;
    
    /* 字符网格 */
    int grid_w = KB_COLS * KB_CELL_W;
    int grid_x = (LCD_H_RES - grid_w) / 2;
    
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(s_kb_container);
            if (!cell) { ESP_LOGE(TAG, "kb_cell failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); continue; }
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, KB_CELL_W - 1, KB_CELL_H - 1);
            lv_obj_set_pos(cell, grid_x + c * KB_CELL_W, y + r * KB_CELL_H);
            lv_obj_set_style_bg_color(cell, lv_color_hex(colors->bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(colors->border), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            
            lv_obj_t *lbl = lv_label_create(cell);
            if (!lbl) { ESP_LOGE(TAG, "kb_lbl failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); continue; }
            const char *ch = kb_get_char(r, c);
            lv_label_set_text(lbl, ch ? ch : "");
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(12), 0);
            lv_obj_center(lbl);
            
            /* OK按钮特殊样式：绿色背景，更醒目 */
            if (r == KB_ROWS - 1 && c == KB_COLS - 1) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(0x22C55E), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            }
            
            s_kb_grid[r][c] = cell;
        }
    }
    y += KB_ROWS * KB_CELL_H + 2;
    
    /* 操作栏 */
    s_kb_action_bar = lv_obj_create(s_kb_container);
    if (!s_kb_action_bar) { ESP_LOGE(TAG, "kb_action_bar failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); return; }
    lv_obj_remove_style_all(s_kb_action_bar);
    lv_obj_set_size(s_kb_action_bar, LCD_H_RES - KB_PADDING * 2, KB_ACTION_H);
    lv_obj_set_pos(s_kb_action_bar, KB_PADDING, y);
    lv_obj_set_style_bg_color(s_kb_action_bar, lv_color_hex(colors->header_bg), 0);
    lv_obj_set_style_bg_opa(s_kb_action_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_kb_action_bar, LV_OBJ_FLAG_SCROLLABLE);
    
    /* 模式切换按钮 */
    s_kb_mode_label = lv_label_create(s_kb_action_bar);
    if (!s_kb_mode_label) { ESP_LOGE(TAG, "kb_mode_label failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); return; }
    lv_label_set_text(s_kb_mode_label, "abc");
    lv_obj_set_style_text_color(s_kb_mode_label, lv_color_hex(colors->sel_bg), 0);
    lv_obj_set_style_text_font(s_kb_mode_label, lv_font_cn_get(12), 0);
    lv_obj_align(s_kb_mode_label, LV_ALIGN_LEFT_MID, 6, 0);
    
    /* 操作提示 */
    lv_obj_t *hint = lv_label_create(s_kb_action_bar);
    if (hint) {
    lv_label_set_text(hint, "A=OK B=←");
    lv_obj_set_style_text_color(hint, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint, lv_font_cn_get(12), 0);
        lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);
    }
    
    kb_refresh_grid();
    
    s_kb_visible = true;
    ESP_LOGI(TAG, "Keyboard shown (title=%s)", config->title ? config->title : "null");
}

void ui_keyboard_hide(void)
{
    if (!s_kb_visible) return;
    
    if (s_kb_container) {
        lv_obj_del(s_kb_container);
        s_kb_container = NULL;
    }
    
    s_kb_input = NULL;
    s_kb_action_bar = NULL;
    s_kb_mode_label = NULL;
    memset(s_kb_grid, 0, sizeof(s_kb_grid));
    
    s_kb_visible = false;
    ESP_LOGI(TAG, "Keyboard hidden");
}

bool ui_keyboard_is_visible(void)
{
    return s_kb_visible;
}

bool ui_keyboard_on_key(int key)
{
    if (!s_kb_visible) return false;
    
    switch (key) {
    case KEY_UP:
        if (s_kb_row > 0) s_kb_row--;
        kb_refresh_grid();
        return true;
        
    case KEY_DOWN:
        if (s_kb_row < KB_ROWS - 1) s_kb_row++;
        kb_refresh_grid();
        return true;
        
    case KEY_LEFT:
        if (s_kb_col > 0) s_kb_col--;
        kb_refresh_grid();
        return true;
        
    case KEY_RIGHT:
        if (s_kb_col < KB_COLS - 1) s_kb_col++;
        kb_refresh_grid();
        return true;
        
    case KEY_A:
        /* 特殊处理：第一行第一列=模式切换 */
        if (s_kb_row == 0 && s_kb_col == 0) {
            s_kb_mode = (kb_mode_t)((s_kb_mode + 1) % KB_MODE_MAX);
            kb_refresh_grid();
            kb_refresh_mode();
            return true;
        }
        /* 第五行最后一列=OK确认按钮 */
        if (s_kb_row == KB_ROWS - 1 && s_kb_col == KB_COLS - 1) {
            kb_confirm();
            return true;
        }
        /* 其他格子：输入字符 */
        {
            const char *ch = kb_get_char(s_kb_row, s_kb_col);
            if (ch) {
                kb_append_char(ch);
            }
        }
        return true;
        
    case KEY_B:
        if (s_kb_text_len > 0) {
            kb_backspace();
        } else {
            kb_cancel();
        }
        return true;
        
    default:
        return false;
    }
}

const char* ui_keyboard_get_text(void)
{
    return s_kb_text;
}

void ui_keyboard_set_text(const char *text)
{
    if (!text) return;
    
    int len = strlen(text);
    int max_len = s_kb_config.max_length > 0 ? s_kb_config.max_length : 64;
    
    if (len >= max_len) {
        len = max_len - 1;
    }
    
    strncpy(s_kb_text, text, len);
    s_kb_text[len] = '\0';
    s_kb_text_len = len;
    
    if (s_kb_visible) {
        kb_refresh_input();
    }
}