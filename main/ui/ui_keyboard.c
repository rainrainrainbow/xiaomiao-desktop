/**
 * @file ui_keyboard.c
 * @brief 虚拟键盘 - 支持 QWERTY 全键盘 + T9 九宫格双布局（适配 160x128 小屏 + 6 键导航）
 *
 * 布局：
 *   顶部：输入框（显示已输入文本）
 *   中间：字符网格（可切换 QWERTY / T9）
 *   底部：操作栏（模式切换/空格/退格/确认）
 *
 * 操作：
 *   UP/DOWN    — 移动行选择
 *   LEFT/RIGHT — 移动列选择
 *   A          — 输入选中字符 / 确认 / 进入 T9 组
 *   B          — 退格 / T9 返回上级 / 返回
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

/* ========== 布局常量 ========== */
#define KB_COLS              7    /* 网格列数（QWERTY 大小写模式按 7 字符一行；T9 = 3列3行） */
#define KB_ROWS              5    /* 网格行数 */
#define KB_T9_COLS           3    /* T9 列数（3） */
#define KB_T9_ROWS           3    /* T9 行数（3） */
#define KB_T9_GROUP_MAX      4    /* T9 组内字符最大数 */
#define KB_CELL_W            22   /* 单元格宽度 */
#define KB_CELL_H            14   /* 单元格高度 */
#define KB_INPUT_H           20   /* 输入框高度 */
#define KB_ACTION_H          18   /* 操作栏高度 */
#define KB_PADDING           2    /* 边距 */
#define KB_TITLE_GAP         14   /* 标题间距 */

/* ========== 键盘状态 ========== */
static bool s_kb_visible = false;
static lv_obj_t *s_kb_container = NULL;
static lv_obj_t *s_kb_input = NULL;
static lv_obj_t *s_kb_grid[KB_ROWS][KB_COLS] = {{NULL}};
static lv_obj_t *s_kb_action_bar = NULL;
static lv_obj_t *s_kb_mode_label = NULL;
static lv_obj_t *s_kb_layout_label = NULL;
static kb_config_t s_kb_config;
static char s_kb_text[128] = {0};
static int s_kb_text_len = 0;
static kb_mode_t s_kb_mode = KB_MODE_LOWER;
static kb_layout_t s_kb_layout = KB_LAYOUT_QWERTY;
static int s_kb_row = 0;
static int s_kb_col = 0;
static int s_kb_t9_inner_idx = 0; /* T9 模式下组内选中索引 */

/* ========== T9 字母组（按手机拨号盘标准顺序） ========== */
static const char *t9_groups[KB_T9_ROWS * KB_T9_COLS] = {
    "abc",   /* [0] */
    "def",   /* [1] */
    "ghi",   /* [2] */
    "jkl",   /* [3] */
    "mno",   /* [4] */
    "pqrs",  /* [5] */
    "tuv",   /* [6] */
    "wxyz",  /* [7] */
    ".,!?",  /* [8] 常用标点 */
};

/* ========== QWERTY 字符表（26字母 + 空格 + 常用标点 + 功能键） ========== */
/* 按行填充，每行 7 个槽位。空槽用 "" 填充。功能键：
 *   "<=": 退格
 *   "123"/"abc"/"ABC"/"#$%": 模式切换
 *   "___": 空格（占3格）
 *   "T9": 切到九宫格
 *   "OK": 确认
 */
static const char *kb_qwerty_lower[KB_ROWS * KB_COLS] = {
    /* Row 0: q-u (7字母) */
    "q","w","e","r","t","y","u",
    /* Row 1: i-f (7字母) */
    "i","o","p","a","s","d","f",
    /* Row 2: g-x (7字母) */
    "g","h","j","k","l","z","x",
    /* Row 3: c-m + 空格(2格) */
    "c","v","b","n","m"," "," ",
    /* Row 4: 模式切换 + 空格(3格) + OK */
    "123"," "," ","OK"," ","<=","T9"
};

static const char *kb_qwerty_upper[KB_ROWS * KB_COLS] = {
    "Q","W","E","R","T","Y","U",
    "I","O","P","A","S","D","F",
    "G","H","J","K","L","Z","X",
    "C","V","B","N","M"," "," ",
    "ABC"," "," ","OK"," ","<=","T9"
};

static const char *kb_qwerty_number[KB_ROWS * KB_COLS] = {
    "1","2","3","4","5","6","7",
    "8","9","0","-","_","=",".",
    "!","@","#","$","%","^","&",
    "*","(",")","[","]"," "," ",
    "#$%"," "," ","OK"," ","<=","T9"
};

static const char *kb_qwerty_symbol[KB_ROWS * KB_COLS] = {
    "~","`","<",">","·","•","©",
    "®","™","€","£","¥","¢","§",
    "±","×","÷","≈","≠","≤","≥",
    "∞","∑","∏","√"," "," "," ",
    "123"," "," ","OK"," ","<=","T9"
};

/* T9 布局：3x3 网格映射到 5x7 的前 3 行 3 列区域。
 * 每个 T9 格子占 (KB_CELL_W*7/3) x (KB_CELL_H) ，自适应放大或保持原尺寸。
 * 这里为简单起见：T9 也使用 KB_CELL_W x KB_CELL_H，但只在前 3 行前 3 列显示。
 * 其余格子显示 OK/退格/空格 等。
 */

/* ========== 辅助：取字符 ========== */
static const char* kb_get_char(int row, int col)
{
    int idx = row * KB_COLS + col;
    if (idx < 0 || idx >= KB_ROWS * KB_COLS) return NULL;

    switch (s_kb_mode) {
    case KB_MODE_LOWER:  return kb_qwerty_lower[idx];
    case KB_MODE_UPPER:  return kb_qwerty_upper[idx];
    case KB_MODE_NUMBER: return kb_qwerty_number[idx];
    case KB_MODE_SYMBOL: return kb_qwerty_symbol[idx];
    default: return NULL;
    }
}

/* T9：取当前选中组的第 i 个字符 */
static const char kb_t9_get_char_in_group(int group_idx, int inner_idx)
{
    const char *grp = t9_groups[group_idx];
    if (!grp || inner_idx < 0 || inner_idx >= (int)strlen(grp)) return '\0';
    return grp[inner_idx];
}

/* ========== 刷新：输入框 ========== */
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

/* ========== 刷新：模式标签 (abc/ABC/123/#$%) ========== */
static void kb_refresh_mode_label(void)
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

/* ========== 刷新：布局标签 (QW / T9) ========== */
static void kb_refresh_layout_label(void)
{
    if (!s_kb_layout_label) return;
    lv_label_set_text(s_kb_layout_label, s_kb_layout == KB_LAYOUT_QWERTY ? "QW" : "T9");
}

/* ========== 刷新：网格文字 + 选中高亮 ========== */
static void kb_refresh_grid(void)
{
    const theme_colors_t *colors = ui_theme_colors();

    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            lv_obj_t *cell = s_kb_grid[r][c];
            if (!cell) continue;

            const char *ch = NULL;
            char t9_buf[5] = {0};

            if (s_kb_layout == KB_LAYOUT_T9) {
                /* T9 模式：前 3 行 3 列 = 9 个字母组 */
                if (r < KB_T9_ROWS && c < KB_T9_COLS) {
                    int gi = r * KB_T9_COLS + c;
                    if (r == s_kb_row && c == s_kb_col) {
                        /* 选中组：展开显示该组字符 (最多 4 个) */
                        const char *grp = t9_groups[gi];
                        if (grp) {
                            /* 复制最多 4 个字符，高亮当前 inner_idx */
                            int glen = (int)strlen(grp);
                            for (int k = 0; k < glen && k < KB_T9_GROUP_MAX; k++) {
                                if (k == s_kb_t9_inner_idx) {
                                    /* 选中字符以 [] 包裹 */
                                    char tmp[3] = {0};
                                    tmp[0] = grp[k]; tmp[1] = '\0';
                                    lv_label_set_text(cell, "");
                                }
                                t9_buf[k] = grp[k];
                            }
                            t9_buf[glen] = '\0';
                            /* 组内光标用 [X] 提示 */
                            char disp[16] = {0};
                            int pos = 0;
                            for (int k = 0; k < glen && k < KB_T9_GROUP_MAX; k++) {
                                if (k == s_kb_t9_inner_idx) {
                                    disp[pos++] = '[';
                                    disp[pos++] = grp[k];
                                    disp[pos++] = ']';
                                } else {
                                    disp[pos++] = grp[k];
                                }
                            }
                            disp[pos] = '\0';
                            lv_label_set_text(cell, disp);
                        }
                    } else {
                        /* 未选中：显示组 */
                        lv_label_set_text(cell, t9_groups[gi]);
                    }
                } else if (r == 3 && c == 0) {
                    lv_label_set_text(cell, "___");  /* 空格占 3 格 */
                } else if (r == 3 && c == 1) {
                    lv_label_set_text(cell, "___");
                } else if (r == 3 && c == 2) {
                    lv_label_set_text(cell, "___");
                } else if (r == 3 && c == 3) {
                    lv_label_set_text(cell, "123"); /* 模式切换：T9下用123切数字/字母 */
                } else if (r == 3 && c == 4) {
                    lv_label_set_text(cell, "QW");  /* 切换到 QWERTY */
                } else if (r == 3 && c == 5) {
                    lv_label_set_text(cell, "<=");
                } else if (r == 4 && c == 6) {
                    lv_label_set_text(cell, "OK");
                } else {
                    lv_label_set_text(cell, "");
                }
            } else {
                /* QWERTY 模式 */
                ch = kb_get_char(r, c);
                if (ch && strlen(ch) > 0) {
                    if (strcmp(ch, "___") == 0) {
                        lv_label_set_text(cell, " ");
                    } else {
                        lv_label_set_text(cell, ch);
                    }
                } else {
                    lv_label_set_text(cell, "");
                }
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

/* ========== 字符输入 ========== */
static void kb_append_char_str(const char *ch)
{
    if (!ch || strlen(ch) == 0) return;

    int ch_len = strlen(ch);
    int max_len = s_kb_config.max_length > 0 ? s_kb_config.max_length : 64;

    if (s_kb_text_len + ch_len >= max_len) {
        ESP_LOGW(TAG, "Input too long (max=%d)", max_len);
        return;
    }

    memcpy(s_kb_text + s_kb_text_len, ch, ch_len);
    s_kb_text_len += ch_len;
    s_kb_text[s_kb_text_len] = '\0';
    kb_refresh_input();
}

static void kb_append_char(char c)
{
    char buf[2] = {c, '\0'};
    kb_append_char_str(buf);
}

static void kb_append_space(void)
{
    kb_append_char(' ');
}

static void kb_backspace(void)
{
    if (s_kb_text_len > 0) {
        /* 处理 UTF-8 多字节字符：一个字符最高 4 字节。
         * 当前用例是英文 + 中文（中文 3 字节 UTF-8）。
         * 简单做法：回退 1 个字节，如果回退后前一个字节是连续字节 (0x80-0xBF)
         * 则继续回退，直到到达字符起始字节 (0x00-0x7F 或 0xC0-0xFF)。
         */
        do {
            s_kb_text[--s_kb_text_len] = '\0';
        } while (s_kb_text_len > 0 && (s_kb_text[s_kb_text_len - 1] & 0xC0) == 0x80);
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

/* ========== T9 专用逻辑 ========== */
/* A 键按下：QWERTY 模式按字符；T9 模式进入组内字符选择（按 inner_idx 输入） */
static void kb_on_a_key(void)
{
    if (s_kb_layout == KB_LAYOUT_T9) {
        int r = s_kb_row, c = s_kb_col;
        /* T9 组：3x3 前 9 格 */
        if (r < KB_T9_ROWS && c < KB_T9_COLS) {
            int gi = r * KB_T9_COLS + c;
            const char *grp = t9_groups[gi];
            if (grp && s_kb_t9_inner_idx < (int)strlen(grp)) {
                kb_append_char(grp[s_kb_t9_inner_idx]);
                /* 输入后保持在该组，方便连续输入该组字符 */
            }
            return;
        }
        /* 第 3 行：空格 */
        if (r == 3 && c >= 0 && c <= 2) {
            kb_append_space();
            return;
        }
        if (r == 3 && c == 3) {
            /* 模式：T9 下切换数字/字母（用 NUMBER/LOWER 简化） */
            if (s_kb_mode == KB_MODE_NUMBER) {
                s_kb_mode = KB_MODE_LOWER;
            } else {
                s_kb_mode = KB_MODE_NUMBER;
            }
            kb_refresh_mode_label();
            kb_refresh_grid();
            return;
        }
        if (r == 3 && c == 4) {
            /* 切到 QWERTY */
            s_kb_layout = KB_LAYOUT_QWERTY;
            s_kb_row = 0; s_kb_col = 0;
            kb_refresh_layout_label();
            kb_refresh_grid();
            return;
        }
        if (r == 3 && c == 5) {
            kb_backspace();
            return;
        }
        if (r == 4 && c == 6) {
            kb_confirm();
            return;
        }
    } else {
        /* QWERTY 模式：A 键 */
        const char *ch = kb_get_char(s_kb_row, s_kb_col);
        if (!ch || strlen(ch) == 0) return;

        if (strcmp(ch, "T9") == 0) {
            /* 切到 T9 */
            s_kb_layout = KB_LAYOUT_T9;
            s_kb_row = 0; s_kb_col = 0;
            s_kb_t9_inner_idx = 0;
            kb_refresh_layout_label();
            kb_refresh_grid();
            return;
        }
        if (strcmp(ch, "OK") == 0) {
            kb_confirm();
            return;
        }
        if (strcmp(ch, "<=") == 0) {
            kb_backspace();
            return;
        }
        /* 空格槽（占位" "或空串""）：输入空格 */
        if (strcmp(ch, " ") == 0 || strlen(ch) == 0) {
            kb_append_space();
            return;
        }
        /* 模式切换键 */
        if (strcmp(ch, "abc") == 0) { s_kb_mode = KB_MODE_LOWER; kb_refresh_mode_label(); kb_refresh_grid(); return; }
        if (strcmp(ch, "ABC") == 0) { s_kb_mode = KB_MODE_UPPER; kb_refresh_mode_label(); kb_refresh_grid(); return; }
        if (strcmp(ch, "123") == 0) { s_kb_mode = KB_MODE_NUMBER; kb_refresh_mode_label(); kb_refresh_grid(); return; }
        if (strcmp(ch, "#$%") == 0) { s_kb_mode = KB_MODE_SYMBOL; kb_refresh_mode_label(); kb_refresh_grid(); return; }

        /* 普通字符 */
        kb_append_char_str(ch);
    }
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
    s_kb_layout = KB_LAYOUT_QWERTY;
    s_kb_row = 0;
    s_kb_col = 0;
    s_kb_t9_inner_idx = 0;

    const theme_colors_t *colors = ui_theme_colors();

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
            y += KB_TITLE_GAP;
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
            lv_obj_set_size(cell, KB_CELL_W - 2, KB_CELL_H);
            lv_obj_set_pos(cell, grid_x + c * KB_CELL_W, y + r * KB_CELL_H);
            lv_obj_set_style_bg_color(cell, lv_color_hex(colors->bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(colors->border), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            if (!lbl) { ESP_LOGE(TAG, "kb_lbl failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); continue; }
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(11), 0);
            lv_obj_center(lbl);

            /* OK 按钮特殊样式：绿色背景 */
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

    /* 布局切换按钮（左：QW/T9） */
    s_kb_layout_label = lv_label_create(s_kb_action_bar);
    if (s_kb_layout_label) {
        lv_obj_set_style_text_color(s_kb_layout_label, lv_color_hex(colors->sel_bg), 0);
        lv_obj_set_style_text_font(s_kb_layout_label, lv_font_cn_get(12), 0);
        lv_label_set_text(s_kb_layout_label, "QW");
        lv_obj_align(s_kb_layout_label, LV_ALIGN_LEFT_MID, 6, 0);
    }

    /* 模式切换按钮（中：abc/ABC/123/#$%） */
    s_kb_mode_label = lv_label_create(s_kb_action_bar);
    if (!s_kb_mode_label) { ESP_LOGE(TAG, "kb_mode_label failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT)); return; }
    lv_label_set_text(s_kb_mode_label, "abc");
    lv_obj_set_style_text_color(s_kb_mode_label, lv_color_hex(colors->sel_bg), 0);
    lv_obj_set_style_text_font(s_kb_mode_label, lv_font_cn_get(12), 0);
    lv_obj_align(s_kb_mode_label, LV_ALIGN_CENTER, 0, 0);

    /* 操作提示（右：A=OK B=BS） */
    lv_obj_t *hint = lv_label_create(s_kb_action_bar);
    if (hint) {
        lv_label_set_text(hint, "A=OK B=BS");
        lv_obj_set_style_text_color(hint, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(hint, lv_font_cn_get(11), 0);
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
    s_kb_layout_label = NULL;
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
        /* T9 模式下，重新选组时重置 inner_idx */
        if (s_kb_layout == KB_LAYOUT_T9) s_kb_t9_inner_idx = 0;
        kb_refresh_grid();
        return true;

    case KEY_DOWN:
        if (s_kb_row < KB_ROWS - 1) s_kb_row++;
        if (s_kb_layout == KB_LAYOUT_T9) s_kb_t9_inner_idx = 0;
        kb_refresh_grid();
        return true;

    case KEY_LEFT:
        if (s_kb_layout == KB_LAYOUT_T9) {
            /* T9 模式：在组内字符间循环 */
            if (s_kb_row < KB_T9_ROWS && s_kb_col < KB_T9_COLS) {
                int gi = s_kb_row * KB_T9_COLS + s_kb_col;
                int glen = (int)strlen(t9_groups[gi]);
                s_kb_t9_inner_idx = (s_kb_t9_inner_idx - 1 + glen) % glen;
            } else {
                if (s_kb_col > 0) s_kb_col--;
            }
        } else {
            if (s_kb_col > 0) s_kb_col--;
        }
        kb_refresh_grid();
        return true;

    case KEY_RIGHT:
        if (s_kb_layout == KB_LAYOUT_T9) {
            if (s_kb_row < KB_T9_ROWS && s_kb_col < KB_T9_COLS) {
                int gi = s_kb_row * KB_T9_COLS + s_kb_col;
                int glen = (int)strlen(t9_groups[gi]);
                s_kb_t9_inner_idx = (s_kb_t9_inner_idx + 1) % glen;
            } else {
                if (s_kb_col < KB_COLS - 1) s_kb_col++;
            }
        } else {
            if (s_kb_col < KB_COLS - 1) s_kb_col++;
        }
        kb_refresh_grid();
        return true;

    case KEY_A:
        kb_on_a_key();
        return true;

    case KEY_B:
        if (s_kb_layout == KB_LAYOUT_T9) {
            /* T9 模式：B 在前 3x3 组内 = 退格（删字符）
             * 在其他位置 = 退出 T9 返回 QWERTY？
             * 设计：A=输入选中，B=退格（与 QWERTY 一致）。
             */
            kb_backspace();
            return true;
        }
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

    memcpy(s_kb_text, text, len);
    s_kb_text[len] = '\0';
    s_kb_text_len = len;

    if (s_kb_visible) {
        kb_refresh_input();
    }
}
