/**
 * @file ui_keyboard.c
 * @brief 虚拟键盘 - 5 键优先布局（默认 T9 九宫格 + 可选 QWERTY），适配 160x128 小屏
 *
 * 设计目标：只用 UP/DOWN/LEFT/RIGHT 四向导航 + A 确认即可高效输入，B 作为全局退格/返回。
 *
 * 布局：
 *   顶部：标题（可选） + 输入框
 *   中部：字符网格（默认 T9 3x3 大键；可切到 QWERTY 6x5）
 *   底部：固定操作栏（布局 T9/QW、模式 abc/ABC/123、A=输入 B=退格 提示）
 *
 * 5 键操作：
 *   UP/DOWN    — 上下移动选中组/行
 *   LEFT/RIGHT — 左右移动选中组/键（T9 字母区在 3 列间移动）
 *   A          — 输入选中字符；T9 下连按 A 在同一组内循环（多输），方向键/空格/确认敲定
 *   B          — 退格（有文本）；无文本时取消（on_cancel）
 *
 * 说明：本键盘输出的是字符（ASCII 拼音/英文字母/数字/符号）。真正的中文候选词需要字库
 *       支持，属于更复杂的输入法范畴，这里不实现；调用方（如 WiFi 密码输入）按字符处理即可。
 */
#include "ui_keyboard.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "UI_KB";

/* ========== 布局常量 ========== */
#define KB_PADDING          2    /* 屏幕边距 */
#define KB_TITLE_GAP        14   /* 标题行高度 */
#define KB_INPUT_H          20   /* 输入框高度 */
#define KB_ACTION_H         18   /* 底部操作栏高度 */
#define KB_GRID_GAP         1    /* QWERTY 格间距 */
#define KB_T9_GAP           2    /* T9 格间距 */
#define KB_T9_FUNC_H        18   /* T9 功能行高度（固定） */
#define KB_T9_MULTI_TAP_MS  1200 /* T9 多输：两次 A 键间隔小于该值视为同一组循环 */

/* QWERTY：6 列 x 5 行（前 4 行 24 字母 + 第 5 行前 2 字母 + 4 功能键） */
#define KB_QW_COLS  6
#define KB_QW_ROWS  5
#define KB_QW_FUNC_START_COL 2  /* 第 5 行从该列起为功能键：模式/空格/T9/OK */

/* T9：3x3 字母组 + 4 个功能键（空格/模式/QW/OK） */
#define KB_T9_COLS       3
#define KB_T9_ROWS       3
#define KB_T9_FUNC_KEYS  4

/* ========== 键盘状态 ========== */
static bool   s_kb_visible   = false;
static kb_config_t s_kb_config;

static lv_obj_t *s_container    = NULL;
static lv_obj_t *s_input        = NULL;
static lv_obj_t *s_action_bar   = NULL;
static lv_obj_t *s_mode_label   = NULL;
static lv_obj_t *s_layout_label = NULL;

static lv_obj_t *s_qw_grid = NULL;
static lv_obj_t *s_qw_cells[KB_QW_ROWS][KB_QW_COLS];
static lv_obj_t *s_t9_grid = NULL;
static lv_obj_t *s_t9_cells[KB_T9_ROWS][KB_T9_COLS];
static lv_obj_t *s_t9_func[KB_T9_FUNC_KEYS];

static char s_kb_text[128] = {0};
static int  s_kb_text_len  = 0;

static kb_mode_t   s_kb_mode   = KB_MODE_LOWER;
static kb_layout_t s_kb_layout = KB_LAYOUT_T9;   /* 默认 T9 九宫格优先 */
static int s_kb_row = 0;
static int s_kb_col = 0;

/* T9 多输（连续按 A 在组内循环）状态：
 * 待定字符已经写入 s_kb_text 末尾；s_pending_pos 记录其位置，-1 表示无待定字符。 */
static int       s_pending_pos    = -1;
static int       s_pending_group  = -1;
static int       s_pending_inner  = 0;
static uint32_t  s_pending_tick   = 0;

/* ========== T9 字母组（手机拨号盘标准顺序，对应数字键 2-9/0） ========== */
static const char *t9_groups[KB_T9_ROWS * KB_T9_COLS] = {
    "abc",   /* 2 */
    "def",   /* 3 */
    "ghi",   /* 4 */
    "jkl",   /* 5 */
    "mno",   /* 6 */
    "pqrs",  /* 7 */
    "tuv",   /* 8 */
    "wxyz",  /* 9 */
    ".,!?",  /* 0 */
};
static const char *t9_digits[KB_T9_ROWS * KB_T9_COLS] = {
    "2", "3", "4", "5", "6", "7", "8", "9", "0",
};

/* ========== QWERTY 字符表 ==========
 * 6 列 x 5 行。第 5 行 col0/1 为字母 o/u，col2..5 为功能键（在刷新时按功能渲染）。 */
static const char *kb_qw_lower[KB_QW_ROWS][KB_QW_COLS] = {
    {"q", "w", "e", "r", "t", "y"},
    {"a", "s", "d", "f", "g", "h"},
    {"i", "j", "k", "l", "z", "x"},
    {"c", "v", "b", "n", "m", "p"},
    {"o", "u", NULL, NULL, NULL, NULL},
};
static const char *kb_qw_upper[KB_QW_ROWS][KB_QW_COLS] = {
    {"Q", "W", "E", "R", "T", "Y"},
    {"A", "S", "D", "F", "G", "H"},
    {"I", "J", "K", "L", "Z", "X"},
    {"C", "V", "B", "N", "M", "P"},
    {"O", "U", NULL, NULL, NULL, NULL},
};
static const char *kb_qw_number[KB_QW_ROWS][KB_QW_COLS] = {
    {"1", "2", "3", "4", "5", "6"},
    {"7", "8", "9", "0", "-", "="},
    {"!", "@", "#", "$", "%", "^"},
    {"&", "*", "(", ")", "_", "+"},
    {".", ",", NULL, NULL, NULL, NULL},
};
static const char *kb_qw_symbol[KB_QW_ROWS][KB_QW_COLS] = {
    {"~", "`", "<", ">", "[", "]"},
    {"{", "}", "/", "\\", "|", ";"},
    {":", "'", "\"", "?", "!", "-"},
    {"+", "=", "(", ")", "*", "%"},
    {"#", "$", NULL, NULL, NULL, NULL},
};

/* QWERTY 第 5 行 4 个功能键的标签：模式 / 空格 / 切 T9 / 确认
 * QWERTY 格子约 25px 宽，故用单字避免换行（T9 功能键更宽，可用双字） */
static const char *kb_qw_func_labels[4] = {"模", "空", "T9", "OK"};
/* T9 功能行 4 个功能键的标签：空格 / 模式 / 切 QW / 确认 */
static const char *kb_t9_func_labels[KB_T9_FUNC_KEYS] = {"空格", "模式", "QW", "OK"};

/* ========== 辅助：取 QWERTY 字符 ========== */
static const char* kb_qw_get_char(int row, int col)
{
    if (row < 0 || row >= KB_QW_ROWS || col < 0 || col >= KB_QW_COLS) return NULL;
    switch (s_kb_mode) {
    case KB_MODE_LOWER:  return kb_qw_lower[row][col];
    case KB_MODE_UPPER:  return kb_qw_upper[row][col];
    case KB_MODE_NUMBER: return kb_qw_number[row][col];
    case KB_MODE_SYMBOL: return kb_qw_symbol[row][col];
    default: return NULL;
    }
}

static void kb_clear_pending(void)
{
    s_pending_pos   = -1;
    s_pending_group = -1;
    s_pending_inner = 0;
}

/* ========== 刷新：输入框 ========== */
static void kb_refresh_input(void)
{
    if (!s_input) return;

    if (s_kb_text_len == 0) {
        lv_label_set_text(s_input, s_kb_config.placeholder ? s_kb_config.placeholder : "");
        const theme_colors_t *colors = ui_theme_colors();
        if (colors) lv_obj_set_style_text_color(s_input, lv_color_hex(colors->text_dim), 0);
    } else {
        if (s_kb_config.password_mode) {
            char masked[128];
            int len = s_kb_text_len < 64 ? s_kb_text_len : 64;
            memset(masked, '*', len);
            masked[len] = '\0';
            lv_label_set_text(s_input, masked);
        } else {
            lv_label_set_text(s_input, s_kb_text);
        }
        const theme_colors_t *colors = ui_theme_colors();
        if (colors) lv_obj_set_style_text_color(s_input, lv_color_hex(colors->text), 0);
    }
}

/* ========== 刷新：模式标签 (abc/ABC/123/#$%) ========== */
static void kb_refresh_mode_label(void)
{
    if (!s_mode_label) return;
    const char *mode_text = "";
    switch (s_kb_mode) {
    case KB_MODE_LOWER:  mode_text = "abc"; break;
    case KB_MODE_UPPER:  mode_text = "ABC"; break;
    case KB_MODE_NUMBER: mode_text = "123"; break;
    case KB_MODE_SYMBOL: mode_text = "#$%"; break;
    default: break;
    }
    lv_label_set_text(s_mode_label, mode_text);
}

/* ========== 刷新：布局标签 (QW / T9) ========== */
static void kb_refresh_layout_label(void)
{
    if (!s_layout_label) return;
    lv_label_set_text(s_layout_label, s_kb_layout == KB_LAYOUT_T9 ? "T9" : "QW");
}

/* ========== 刷新：QWERTY 网格文字 + 选中高亮 ========== */
static void kb_refresh_qw(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    uint32_t bg    = colors ? colors->bg     : 0x000000;
    uint32_t selbg = colors ? colors->sel_bg : 0x444444;

    for (int r = 0; r < KB_QW_ROWS; r++) {
        for (int c = 0; c < KB_QW_COLS; c++) {
            lv_obj_t *cell = s_qw_cells[r][c];
            if (!cell) continue;

            lv_obj_t *lbl = lv_obj_get_child(cell, 0);
            const char *ch = NULL;

            if (r == KB_QW_ROWS - 1 && c >= KB_QW_FUNC_START_COL) {
                /* 功能键列 */
                ch = kb_qw_func_labels[c - KB_QW_FUNC_START_COL];
            } else {
                ch = kb_qw_get_char(r, c);
            }
            if (lbl) lv_label_set_text(lbl, ch ? ch : "");

            if (r == s_kb_row && c == s_kb_col) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(selbg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            }
        }
    }
}

/* ========== 刷新：T9 网格文字 + 选中高亮 ========== */
static void kb_refresh_t9(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    uint32_t bg    = colors ? colors->bg     : 0x000000;
    uint32_t selbg = colors ? colors->sel_bg : 0x444444;

    /* 字母区 3x3 */
    for (int r = 0; r < KB_T9_ROWS; r++) {
        for (int c = 0; c < KB_T9_COLS; c++) {
            lv_obj_t *cell = s_t9_cells[r][c];
            if (!cell) continue;
            lv_obj_t *lbl = lv_obj_get_child(cell, 0);
            int gi = r * KB_T9_COLS + c;
            const char *grp = t9_groups[gi];
            char disp[24] = {0};

            if (s_kb_mode == KB_MODE_NUMBER) {
                /* 数字模式：显示对应数字 */
                snprintf(disp, sizeof(disp), "%s", t9_digits[gi]);
            } else if (r == s_kb_row && c == s_kb_col) {
                /* 选中组：展开组内字符，当前循环字符用 [] 包裹 */
                int glen = (int)strlen(grp);
                int inner = (s_pending_group == gi) ? s_pending_inner : 0;
                if (inner >= glen) inner = 0;
                int pos = 0;
                for (int k = 0; k < glen && pos + 4 < (int)sizeof(disp); k++) {
                    char ch = grp[k];
                    if (s_kb_mode == KB_MODE_UPPER && ch >= 'a' && ch <= 'z') ch -= 32;
                    if (k == inner) {
                        disp[pos++] = '[';
                        disp[pos++] = ch;
                        disp[pos++] = ']';
                    } else {
                        disp[pos++] = ch;
                    }
                }
                disp[pos] = '\0';
            } else {
                snprintf(disp, sizeof(disp), "%s", grp);
            }
            if (lbl) lv_label_set_text(lbl, disp);

            if (r == s_kb_row && c == s_kb_col) {
                lv_obj_set_style_bg_color(cell, lv_color_hex(selbg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            }
        }
    }

    /* 功能行 */
    for (int i = 0; i < KB_T9_FUNC_KEYS; i++) {
        lv_obj_t *cell = s_t9_func[i];
        if (!cell) continue;
        if (s_kb_row == KB_T9_ROWS && s_kb_col == i) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(selbg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
        }
    }
}

/* ========== 刷新：网格（切换两套网格的显示 + 重绘文字） ========== */
static void kb_refresh_grid(void)
{
    if (s_kb_layout == KB_LAYOUT_T9) {
        if (s_qw_grid) lv_obj_add_flag(s_qw_grid, LV_OBJ_FLAG_HIDDEN);
        if (s_t9_grid) lv_obj_clear_flag(s_t9_grid, LV_OBJ_FLAG_HIDDEN);
        kb_refresh_t9();
    } else {
        if (s_t9_grid) lv_obj_add_flag(s_t9_grid, LV_OBJ_FLAG_HIDDEN);
        if (s_qw_grid) lv_obj_clear_flag(s_qw_grid, LV_OBJ_FLAG_HIDDEN);
        kb_refresh_qw();
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
    kb_clear_pending();
    kb_append_char(' ');
}

static void kb_backspace(void)
{
    kb_clear_pending();
    if (s_kb_text_len > 0) {
        /* 回退一个 UTF-8 字符：多字节从末尾连续字节回退到起始字节 */
        do {
            s_kb_text[--s_kb_text_len] = '\0';
        } while (s_kb_text_len > 0 && (s_kb_text[s_kb_text_len - 1] & 0xC0) == 0x80);
        kb_refresh_input();
    }
}

static void kb_confirm(void)
{
    kb_clear_pending();
    if (s_kb_config.on_confirm) {
        s_kb_config.on_confirm(s_kb_text, s_kb_config.user_data);
    }
    ui_keyboard_hide();
}

static void kb_cancel(void)
{
    kb_clear_pending();
    if (s_kb_config.on_cancel) {
        s_kb_config.on_cancel(s_kb_config.user_data);
    }
    ui_keyboard_hide();
}

/* ========== 模式/布局切换 ========== */
static void kb_cycle_mode(void)
{
    kb_clear_pending();
    s_kb_mode = (kb_mode_t)((s_kb_mode + 1) % KB_MODE_MAX);
    kb_refresh_mode_label();
    kb_refresh_grid();
}

/* T9 模式循环：小写字母 → 大写字母 → 数字 → 小写（九宫格不需要符号页） */
static void kb_t9_cycle_mode(void)
{
    kb_clear_pending();
    if (s_kb_mode == KB_MODE_LOWER)      s_kb_mode = KB_MODE_UPPER;
    else if (s_kb_mode == KB_MODE_UPPER) s_kb_mode = KB_MODE_NUMBER;
    else                                  s_kb_mode = KB_MODE_LOWER;
    kb_refresh_mode_label();
    kb_refresh_grid();
}

static void kb_switch_layout(kb_layout_t layout)
{
    if (s_kb_layout == layout) return;
    kb_clear_pending();
    s_kb_layout = layout;
    /* T9 无符号页：从 QWERTY 的符号模式切回来时回退到小写字母 */
    if (layout == KB_LAYOUT_T9 && s_kb_mode == KB_MODE_SYMBOL) {
        s_kb_mode = KB_MODE_LOWER;
        kb_refresh_mode_label();
    }
    s_kb_row = 0;
    s_kb_col = 0;
    kb_refresh_layout_label();
    kb_refresh_grid();
}

/* ========== T9 多输：A 键处理 ========== */
static void kb_t9_on_a(void)
{
    /* 字母区 3x3 */
    if (s_kb_row < KB_T9_ROWS && s_kb_col < KB_T9_COLS) {
        int gi = s_kb_row * KB_T9_COLS + s_kb_col;

        /* 数字模式：直接输入对应数字，无多输循环 */
        if (s_kb_mode == KB_MODE_NUMBER) {
            kb_clear_pending();
            kb_append_char_str(t9_digits[gi]);
            return;
        }

        const char *grp = t9_groups[gi];
        int glen = (int)strlen(grp);
        uint32_t now = lv_tick_get();

        /* 连续按同一组且在超时窗口内：替换末尾待定字符为组内下一个字符 */
        if (s_pending_group == gi && s_pending_pos >= s_kb_text_len - 1 &&
            (now - s_pending_tick) <= KB_T9_MULTI_TAP_MS) {
            s_pending_inner = (s_pending_inner + 1) % glen;
            char ch = grp[s_pending_inner];
            if (s_kb_mode == KB_MODE_UPPER && ch >= 'a' && ch <= 'z') ch -= 32;
            s_kb_text[s_pending_pos] = ch;
            s_pending_tick = now;
            kb_refresh_input();
            kb_refresh_t9();
            return;
        }

        /* 新输入：追加组内第 0 个字符，并置为待定。
         * 只有真正追加成功才置 pending，避免长度已满时误把末尾别人的字符当待定字符循环。 */
        kb_clear_pending();
        char ch = grp[0];
        if (s_kb_mode == KB_MODE_UPPER && ch >= 'a' && ch <= 'z') ch -= 32;
        int old_len = s_kb_text_len;
        kb_append_char(ch);
        if (s_kb_text_len > old_len) {
            s_pending_pos   = s_kb_text_len - 1;
            s_pending_group = gi;
            s_pending_inner = 0;
            s_pending_tick  = now;
        }
        kb_refresh_t9();
        return;
    }

    /* 功能行 */
    if (s_kb_row == KB_T9_ROWS) {
        switch (s_kb_col) {
        case 0: kb_append_space(); break;           /* 空格 */
        case 1: kb_t9_cycle_mode(); break;          /* 模式：abc/ABC/123 循环 */
        case 2: kb_switch_layout(KB_LAYOUT_QWERTY); break; /* QW */
        case 3: kb_confirm();     break;            /* OK */
        default: break;
        }
    }
}

/* ========== 功能键动作（QWERTY 第 5 行） ========== */
static void kb_qw_on_func(int idx)
{
    switch (idx) {
    case 0: kb_cycle_mode();  break;             /* MODE */
    case 1: kb_append_space(); break;            /* SPACE */
    case 2: kb_switch_layout(KB_LAYOUT_T9); break; /* T9 */
    case 3: kb_confirm();     break;             /* OK */
    default: break;
    }
}

/* ========== 公共接口 ========== */
static void kb_destroy_views(void)
{
    if (s_container) {
        lv_obj_del(s_container);
        s_container = NULL;
    }
    s_input = NULL;
    s_action_bar = NULL;
    s_mode_label = NULL;
    s_layout_label = NULL;
    memset(s_qw_cells, 0, sizeof(s_qw_cells));
    memset(s_t9_cells, 0, sizeof(s_t9_cells));
    memset(s_t9_func, 0, sizeof(s_t9_func));
    s_qw_grid = NULL;
    s_t9_grid = NULL;
}

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
    s_kb_layout = KB_LAYOUT_T9;   /* 默认九宫格优先 */
    s_kb_row = 0;
    s_kb_col = 0;
    kb_clear_pending();

    const theme_colors_t *colors = ui_theme_colors();
    uint32_t c_bg     = colors ? colors->bg        : 0x000000;
    uint32_t c_text   = colors ? colors->text      : 0xFFFFFF;
    uint32_t c_dim    = colors ? colors->text_dim  : 0x888888;
    uint32_t c_header = colors ? colors->header_bg : 0x222222;
    uint32_t c_border = colors ? colors->border    : 0x555555;
    uint32_t c_sel    = colors ? colors->sel_bg    : 0x444444;

    /* 全屏容器 */
    s_container = lv_obj_create(lv_screen_active());
    if (!s_container) {
        ESP_LOGE(TAG, "kb_container failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_container);
    lv_obj_set_size(s_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);

    int y = KB_PADDING;

    /* 标题 */
    if (config->title) {
        lv_obj_t *title = lv_label_create(s_container);
        if (title) {
            lv_label_set_text(title, config->title);
            lv_obj_set_style_text_color(title, lv_color_hex(c_text), 0);
            lv_obj_set_style_text_font(title, lv_font_cn_get(14), 0);
            lv_obj_set_pos(title, KB_PADDING, y);
        }
        y += KB_TITLE_GAP;
    }

    /* 输入框 */
    s_input = lv_label_create(s_container);
    if (!s_input) {
        ESP_LOGE(TAG, "kb_input failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        kb_destroy_views();
        return;
    }
    lv_obj_set_size(s_input, LCD_H_RES - KB_PADDING * 2, KB_INPUT_H);
    lv_obj_set_pos(s_input, KB_PADDING, y);
    lv_obj_set_style_bg_color(s_input, lv_color_hex(c_header), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_input, 4, 0);
    lv_obj_set_style_pad_left(s_input, 6, 0);
    lv_obj_set_style_pad_right(s_input, 6, 0);
    lv_obj_set_style_pad_top(s_input, 2, 0);
    lv_obj_set_style_text_font(s_input, lv_font_cn_get(12), 0);
    lv_label_set_long_mode(s_input, LV_LABEL_LONG_SCROLL_CIRCULAR);
    kb_refresh_input();
    y += KB_INPUT_H + KB_GRID_GAP;

    /* 网格区域（两套网格共用同一矩形，靠 hidden 切换） */
    int grid_w = LCD_H_RES - KB_PADDING * 2;
    int grid_h = LCD_V_RES - y - KB_ACTION_H - KB_PADDING - KB_GRID_GAP;
    if (grid_h < 40) grid_h = 40;
    int grid_x = KB_PADDING;
    int grid_y = y;

    /* ---- QWERTY 网格：6 列 x 5 行 ---- */
    s_qw_grid = lv_obj_create(s_container);
    if (!s_qw_grid) {
        ESP_LOGE(TAG, "qw_grid failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        kb_destroy_views();
        return;
    }
    lv_obj_remove_style_all(s_qw_grid);
    lv_obj_set_size(s_qw_grid, grid_w, grid_h);
    lv_obj_set_pos(s_qw_grid, grid_x, grid_y);
    lv_obj_clear_flag(s_qw_grid, LV_OBJ_FLAG_SCROLLABLE);

    int qw_cw = (grid_w - (KB_QW_COLS - 1) * KB_GRID_GAP) / KB_QW_COLS;
    int qw_ch = (grid_h - (KB_QW_ROWS - 1) * KB_GRID_GAP) / KB_QW_ROWS;
    for (int r = 0; r < KB_QW_ROWS; r++) {
        for (int c = 0; c < KB_QW_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(s_qw_grid);
            if (!cell) {
                ESP_LOGE(TAG, "qw_cell[%d][%d] failed! mem free=%lu", r, c,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, qw_cw, qw_ch);
            lv_obj_set_pos(cell, c * (qw_cw + KB_GRID_GAP), r * (qw_ch + KB_GRID_GAP));
            lv_obj_set_style_bg_color(cell, lv_color_hex(c_bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            lv_obj_set_style_radius(cell, 2, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(c_border), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(c_text), 0);
                lv_obj_set_style_text_font(lbl, lv_font_cn_get(11), 0);
                lv_obj_center(lbl);
            }
            s_qw_cells[r][c] = cell;
        }
    }

    /* ---- T9 网格：3x3 大字母键 + 4 功能键 ---- */
    s_t9_grid = lv_obj_create(s_container);
    if (!s_t9_grid) {
        ESP_LOGE(TAG, "t9_grid failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        kb_destroy_views();
        return;
    }
    lv_obj_remove_style_all(s_t9_grid);
    lv_obj_set_size(s_t9_grid, grid_w, grid_h);
    lv_obj_set_pos(s_t9_grid, grid_x, grid_y);
    lv_obj_clear_flag(s_t9_grid, LV_OBJ_FLAG_SCROLLABLE);

    int t9_cw = (grid_w - (KB_T9_COLS - 1) * KB_T9_GAP) / KB_T9_COLS;
    /* 3 行字母（行间 2*gap）+ 1 行字母/功能间隔（gap）+ 功能行 = grid_h */
    int t9_ch = (grid_h - KB_T9_FUNC_H - 3 * KB_T9_GAP) / KB_T9_ROWS;
    if (t9_ch < 12) t9_ch = 12;

    for (int r = 0; r < KB_T9_ROWS; r++) {
        for (int c = 0; c < KB_T9_COLS; c++) {
            lv_obj_t *cell = lv_obj_create(s_t9_grid);
            if (!cell) {
                ESP_LOGE(TAG, "t9_cell[%d][%d] failed! mem free=%lu", r, c,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                continue;
            }
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, t9_cw, t9_ch);
            lv_obj_set_pos(cell, c * (t9_cw + KB_T9_GAP), r * (t9_ch + KB_T9_GAP));
            lv_obj_set_style_bg_color(cell, lv_color_hex(c_bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_70, 0);
            lv_obj_set_style_radius(cell, 3, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(c_border), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, lv_color_hex(c_text), 0);
                lv_obj_set_style_text_font(lbl, lv_font_cn_get(12), 0);
                lv_obj_center(lbl);
            }
            s_t9_cells[r][c] = cell;
        }
    }

    /* T9 功能行：4 键平分宽度 */
    int func_w = (grid_w - (KB_T9_FUNC_KEYS - 1) * KB_T9_GAP) / KB_T9_FUNC_KEYS;
    int func_y = KB_T9_ROWS * (t9_ch + KB_T9_GAP);
    for (int i = 0; i < KB_T9_FUNC_KEYS; i++) {
        lv_obj_t *cell = lv_obj_create(s_t9_grid);
        if (!cell) {
            ESP_LOGE(TAG, "t9_func[%d] failed! mem free=%lu", i,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, func_w, KB_T9_FUNC_H);
        lv_obj_set_pos(cell, i * (func_w + KB_T9_GAP), func_y);
        lv_obj_set_style_bg_color(cell, lv_color_hex(c_header), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 3, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(c_border), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(cell);
        if (lbl) {
            lv_label_set_text(lbl, kb_t9_func_labels[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(i == 3 ? 0xFFFFFF : c_text), 0);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(14), 0);
            lv_obj_center(lbl);
        }
        /* OK 键绿色背景 */
        if (i == 3) lv_obj_set_style_bg_color(cell, lv_color_hex(0x22C55E), 0);
        s_t9_func[i] = cell;
    }

    y += grid_h + KB_GRID_GAP;

    /* 底部操作栏 */
    s_action_bar = lv_obj_create(s_container);
    if (!s_action_bar) {
        ESP_LOGE(TAG, "action_bar failed! mem free=%lu", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        kb_destroy_views();
        return;
    }
    lv_obj_remove_style_all(s_action_bar);
    lv_obj_set_size(s_action_bar, LCD_H_RES - KB_PADDING * 2, KB_ACTION_H);
    lv_obj_set_pos(s_action_bar, KB_PADDING, y);
    lv_obj_set_style_bg_color(s_action_bar, lv_color_hex(c_header), 0);
    lv_obj_set_style_bg_opa(s_action_bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_action_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_layout_label = lv_label_create(s_action_bar);
    if (s_layout_label) {
        lv_obj_set_style_text_color(s_layout_label, lv_color_hex(c_sel), 0);
        lv_obj_set_style_text_font(s_layout_label, lv_font_cn_get(12), 0);
        lv_obj_align(s_layout_label, LV_ALIGN_LEFT_MID, 6, 0);
    }

    s_mode_label = lv_label_create(s_action_bar);
    if (s_mode_label) {
        lv_obj_set_style_text_color(s_mode_label, lv_color_hex(c_sel), 0);
        lv_obj_set_style_text_font(s_mode_label, lv_font_cn_get(12), 0);
        lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t *hint = lv_label_create(s_action_bar);
    if (hint) {
        lv_label_set_text(hint, "A=输入 B=退格");
        lv_obj_set_style_text_color(hint, lv_color_hex(c_dim), 0);
        lv_obj_set_style_text_font(hint, lv_font_cn_get(14), 0);
        lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);
    }

    kb_refresh_mode_label();
    kb_refresh_layout_label();
    kb_refresh_grid();

    s_kb_visible = true;
    ESP_LOGI(TAG, "Keyboard shown (layout=T9, title=%s)", config->title ? config->title : "null");
}

void ui_keyboard_hide(void)
{
    if (!s_kb_visible) return;
    kb_clear_pending();
    kb_destroy_views();
    s_kb_visible = false;
    ESP_LOGI(TAG, "Keyboard hidden");
}

bool ui_keyboard_is_visible(void)
{
    return s_kb_visible;
}

/* 当前布局下，第 row 行的最大列号 */
static int kb_max_col(int row)
{
    if (s_kb_layout == KB_LAYOUT_T9) {
        return (row == KB_T9_ROWS) ? (KB_T9_FUNC_KEYS - 1) : (KB_T9_COLS - 1);
    }
    return KB_QW_COLS - 1;
}

/* 当前布局下的最大行号 */
static int kb_max_row(void)
{
    return (s_kb_layout == KB_LAYOUT_T9) ? KB_T9_ROWS : (KB_QW_ROWS - 1);
}

/* 方向键移动会敲定当前多输字符 */
static void kb_commit_pending(void)
{
    kb_clear_pending();
}

bool ui_keyboard_on_key(int key)
{
    if (!s_kb_visible) return false;

    switch (key) {
    case KEY_UP:
        if (s_kb_row > 0) s_kb_row--;
        if (s_kb_col > kb_max_col(s_kb_row)) s_kb_col = kb_max_col(s_kb_row);
        kb_commit_pending();
        kb_refresh_grid();
        return true;

    case KEY_DOWN:
        if (s_kb_row < kb_max_row()) s_kb_row++;
        if (s_kb_col > kb_max_col(s_kb_row)) s_kb_col = kb_max_col(s_kb_row);
        kb_commit_pending();
        kb_refresh_grid();
        return true;

    case KEY_LEFT:
        if (s_kb_col > 0) s_kb_col--;
        kb_commit_pending();
        kb_refresh_grid();
        return true;

    case KEY_RIGHT:
        if (s_kb_col < kb_max_col(s_kb_row)) s_kb_col++;
        kb_commit_pending();
        kb_refresh_grid();
        return true;

    case KEY_A:
        if (s_kb_layout == KB_LAYOUT_T9) {
            kb_t9_on_a();
        } else if (s_kb_row == KB_QW_ROWS - 1 && s_kb_col >= KB_QW_FUNC_START_COL) {
            kb_qw_on_func(s_kb_col - KB_QW_FUNC_START_COL);
        } else {
            kb_commit_pending();
            const char *ch = kb_qw_get_char(s_kb_row, s_kb_col);
            if (ch && strlen(ch) > 0) kb_append_char_str(ch);
        }
        return true;

    case KEY_B:
        /* B 全局退格；无文本时取消 */
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

    if (len >= max_len) len = max_len - 1;

    memcpy(s_kb_text, text, len);
    s_kb_text[len] = '\0';
    s_kb_text_len = len;
    kb_clear_pending();

    if (s_kb_visible) kb_refresh_input();
}
