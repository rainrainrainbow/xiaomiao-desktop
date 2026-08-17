/**
 * @file app_editor.c
 * @brief 积木编辑器应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_editor_callbacks。
 * 参考 LiClock 的 App 架构设计，每个 App 独立文件。
 * 功能：编辑积木序列，支持事件/运动/外观/控制/运算/变量/声音分类，
 *       可将积木序列转换为MicroPython代码并执行。
 */
#include "app_builtin.h"
#include "app_micropython.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_EDITOR";

/* ========== 积木数据 ========== */
#define BLOCK_CAT_COUNT 7
static const char *s_block_cats[BLOCK_CAT_COUNT] = {
    "事件", "运动", "外观", "控制", "运算", "变量", "声音"
};

#define BLOCKS_PER_CAT 4
static const char *s_block_names[BLOCK_CAT_COUNT][BLOCKS_PER_CAT] = {
    {"当启动时", "当按键按下", "当收到消息", "重复执行"},
    {"移动10", "转向15", "移到随机", "滑行1秒"},
    {"说你好", "显示", "隐藏", "切换造型"},
    {"等待1秒", "重复10次", "如果那么", "停止"},
    {"加", "减", "乘", "取余"},
    {"设变量", "变量+1", "显示变量", "清空变量"},
    {"播放音效", "播放旋律", "静音", "音量50"},
};

static const int s_block_params[BLOCK_CAT_COUNT][BLOCKS_PER_CAT] = {
    {-1, -1, -1, -1}, {10, 15, -1, 1}, {-1, -1, -1, -1}, {1, 10, -1, -1},
    {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, 50},
};

static const bool s_block_has_param[BLOCK_CAT_COUNT][BLOCKS_PER_CAT] = {
    {false, false, false, false}, {true, true, false, true}, {false, false, false, false},
    {true, true, false, false}, {false, false, false, false},
    {false, false, false, false}, {false, false, false, true},
};

#define MAX_PROG_BLOCKS 16

typedef struct {
    int block_idx;
    int param_val;
} prog_block_t;

static lv_obj_t *s_editor_obj = NULL;
static int s_editor_pane = 0;
static int s_editor_cat_sel = 0;
static int s_editor_block_sel = 0;
static int s_editor_prog_sel = 0;
static int s_editor_prog_count = 0;
static prog_block_t s_editor_prog_blocks[MAX_PROG_BLOCKS];
static lv_obj_t *s_editor_pane_l = NULL;
static lv_obj_t *s_editor_pane_r = NULL;
static int s_editor_prog_mode = 0;
static int s_editor_prog_menu_sel = 0;
static int s_editor_param_mode = 0;
static int s_editor_param_val = 0;
static int s_editor_param_min = 0;
static int s_editor_param_max = 0;
static int s_editor_run_mode = 0;   // 1=运行中

static uint32_t s_cat_colors[BLOCK_CAT_COUNT] = {
    0xE64B3C, 0x22C55E, 0x3B82F6, 0xE64B3C, 0xF59E0B, 0x8B5CF6, 0x06B6D4,
};

static void editor_get_block_display_name(int cat, int blk, int param, char *buf, int buf_size)
{
    const char *name = s_block_names[cat][blk];
    if (s_block_has_param[cat][blk] && param >= 0) {
        snprintf(buf, buf_size, "%s", name);
        char *p = buf;
        while (*p) {
            if (*p >= '0' && *p <= '9') {
                char suffix[16] = "";
                char *q = p;
                while (*q >= '0' && *q <= '9') q++;
                strcpy(suffix, q);
                snprintf(p, buf_size - (p - buf), "%d%s", param, suffix);
                break;
            }
            p++;
        }
    } else {
        snprintf(buf, buf_size, "%s", name);
    }
}

/* ========== 生成MicroPython代码 ========== */

/** 将积木序列转换为MicroPython代码 */
static void editor_generate_code(char *buf, int buf_size)
{
    int pos = 0;
    buf[0] = '\0';
    
    for (int i = 0; i < s_editor_prog_count; i++) {
        int idx = s_editor_prog_blocks[i].block_idx;
        int cat = idx / BLOCKS_PER_CAT;
        int blk = idx % BLOCKS_PER_CAT;
        int param = s_editor_prog_blocks[i].param_val;
        int remaining = buf_size - pos - 1;
        if (remaining <= 0) break;
        
        if (cat == 0) {  // 事件
            if (blk == 0) {  // 当启动时
                pos += snprintf(buf + pos, remaining, "print('启动')\n");
            } else if (blk == 1) {  // 当按键按下
                pos += snprintf(buf + pos, remaining, "print('按键')\n");
            } else if (blk == 2) {  // 当收到消息
                pos += snprintf(buf + pos, remaining, "print('消息')\n");
            } else {  // 重复执行
                pos += snprintf(buf + pos, remaining, "while True:\n");
            }
        } else if (cat == 1) {  // 运动
            if (blk == 0) {  // 移动
                pos += snprintf(buf + pos, remaining, "print('移动%d')\n", param);
            } else if (blk == 1) {  // 转向
                pos += snprintf(buf + pos, remaining, "print('转向%d')\n", param);
            } else if (blk == 2) {  // 移到随机
                pos += snprintf(buf + pos, remaining, "print('随机位置')\n");
            } else {  // 滑行
                pos += snprintf(buf + pos, remaining, "print('滑行%d秒')\n", param);
            }
        } else if (cat == 2) {  // 外观
            if (blk == 0) {  // 说
                pos += snprintf(buf + pos, remaining, "print('你好')\n");
            } else if (blk == 1) {  // 显示
                pos += snprintf(buf + pos, remaining, "print('显示')\n");
            } else if (blk == 2) {  // 隐藏
                pos += snprintf(buf + pos, remaining, "print('隐藏')\n");
            } else {  // 切换造型
                pos += snprintf(buf + pos, remaining, "print('切换造型')\n");
            }
        } else if (cat == 3) {  // 控制
            if (blk == 0) {  // 等待
                pos += snprintf(buf + pos, remaining, "import time\ntime.sleep(%d)\n", param);
            } else if (blk == 1) {  // 重复
                pos += snprintf(buf + pos, remaining, "for _ in range(%d):\n", param);
            } else if (blk == 2) {  // 如果那么
                pos += snprintf(buf + pos, remaining, "if True:\n");
            } else {  // 停止
                pos += snprintf(buf + pos, remaining, "break\n");
            }
        } else if (cat == 4) {  // 运算
            if (blk == 0) {  // 加
                pos += snprintf(buf + pos, remaining, "print(1+1)\n");
            } else if (blk == 1) {  // 减
                pos += snprintf(buf + pos, remaining, "print(2-1)\n");
            } else if (blk == 2) {  // 乘
                pos += snprintf(buf + pos, remaining, "print(2*3)\n");
            } else {  // 取余
                pos += snprintf(buf + pos, remaining, "print(7%%2)\n");
            }
        } else if (cat == 5) {  // 变量
            if (blk == 0) {  // 设变量
                pos += snprintf(buf + pos, remaining, "x = 0\n");
            } else if (blk == 1) {  // 变量+1
                pos += snprintf(buf + pos, remaining, "x = x + 1\nprint(x)\n");
            } else if (blk == 2) {  // 显示变量
                pos += snprintf(buf + pos, remaining, "print(x)\n");
            } else {  // 清空变量
                pos += snprintf(buf + pos, remaining, "x = 0\n");
            }
        } else if (cat == 6) {  // 声音
            if (blk == 0) {  // 播放音效
                pos += snprintf(buf + pos, remaining, "print('播放音效')\n");
            } else if (blk == 1) {  // 播放旋律
                pos += snprintf(buf + pos, remaining, "print('播放旋律')\n");
            } else if (blk == 2) {  // 静音
                pos += snprintf(buf + pos, remaining, "print('静音')\n");
            } else {  // 音量
                pos += snprintf(buf + pos, remaining, "print('音量%d')\n", param);
            }
        }
    }
    
    // 确保有输出
    if (s_editor_prog_count == 0) {
        snprintf(buf, buf_size, "print('空程序')\n");
    }
}

/* ========== 运行程序 ========== */

static void editor_run_program(void)
{
    if (s_editor_run_mode) {
        // 停止运行
        s_editor_run_mode = 0;
        ui_statusbar_set_title("积木编辑器");
        return;
    }
    
    if (s_editor_prog_count == 0) return;
    
    char code[1024];
    editor_generate_code(code, sizeof(code));
    ESP_LOGI(TAG, "Running program:\n%s", code);
    
    s_editor_run_mode = 1;
    ui_statusbar_set_title("运行中...");
    
    // 执行MicroPython代码
    int ret = app_micropython_exec(code, "<blockly>");
    if (ret != 0) {
        ESP_LOGE(TAG, "Program execution failed");
    }
    
    s_editor_run_mode = 0;
    ui_statusbar_set_title("积木编辑器");
}

static void editor_refresh_pane_l(void)
{
    if (!s_editor_pane_l) return;
    lv_obj_clean(s_editor_pane_l);
    const theme_colors_t *colors = ui_theme_colors();
    // 行高和字体根据设置自适应
    int font_px = ui_state_get()->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 2;
    char cat_buf[32];
    for (int i = 0; i < BLOCK_CAT_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_editor_pane_l);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 76, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_t *lbl = lv_label_create(row);
        if (i == s_editor_cat_sel) {
            snprintf(cat_buf, sizeof(cat_buf), ">%s<", s_block_cats[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xE64B3C), 0);
        } else {
            snprintf(cat_buf, sizeof(cat_buf), " %s ", s_block_cats[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(s_cat_colors[i]), 0);
        }
        lv_label_set_text(lbl, cat_buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, 76);
    }
    lv_obj_t *sep = lv_label_create(s_editor_pane_l);
    lv_label_set_text(sep, "────────");
    lv_obj_set_style_text_color(sep, lv_color_hex(colors->border), 0);
    lv_obj_set_width(sep, 76);
    for (int i = 0; i < BLOCKS_PER_CAT; i++) {
        lv_obj_t *row = lv_obj_create(s_editor_pane_l);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 76, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *lbl = lv_label_create(row);
        if (i == s_editor_block_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        }
        char block_buf[24];
        editor_get_block_display_name(s_editor_cat_sel, i,
            s_block_params[s_editor_cat_sel][i], block_buf, sizeof(block_buf));
        lv_label_set_text(lbl, block_buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, 76);
    }
}
static void editor_refresh_pane_r(void)
{
    if (!s_editor_pane_r) return;
    lv_obj_clean(s_editor_pane_r);
    const theme_colors_t *colors = ui_theme_colors();
    // 行高和字体根据设置自适应
    int font_px = ui_state_get()->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 2;
    if (s_editor_prog_mode == 1) {
        const char *menu_items[] = {"运行", "删除", "上移", "下移"};
        int menu_count = sizeof(menu_items) / sizeof(menu_items[0]);
        for (int i = 0; i < menu_count; i++) {
            lv_obj_t *row = lv_obj_create(s_editor_pane_r);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, 76, row_h);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *lbl = lv_label_create(row);
            if (i == s_editor_prog_menu_sel) {
                lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            } else {
                lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            }
            lv_label_set_text(lbl, menu_items[i]);
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl, 76);
        }
        return;
    }
    if (s_editor_prog_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_editor_pane_r);
        lv_label_set_text(lbl, "空");
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, 76);
        return;
    }
    for (int i = 0; i < s_editor_prog_count; i++) {
        int idx = s_editor_prog_blocks[i].block_idx;
        int cat = idx / BLOCKS_PER_CAT;
        int blk = idx % BLOCKS_PER_CAT;
        lv_obj_t *row = lv_obj_create(s_editor_pane_r);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 76, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        char buf[24];
        char name_buf[20];
        editor_get_block_display_name(cat, blk, s_editor_prog_blocks[i].param_val, name_buf, sizeof(name_buf));
        snprintf(buf, sizeof(buf), "%d.%s", i + 1, name_buf);
        lv_obj_t *lbl = lv_label_create(row);
        if (i == s_editor_prog_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(s_cat_colors[cat]), 0);
        }
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, 76);
    }
}

static void editor_init(void *data)
{
    ESP_LOGI(TAG, "Editor init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("积木编辑器");
    lv_obj_t *split = lv_obj_create(scr);
    lv_obj_remove_style_all(split);
    lv_obj_set_pos(split, 0, ui_content_y());
    lv_obj_set_size(split, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(split, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(split, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(split, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *pane_l = lv_obj_create(split);
    lv_obj_remove_style_all(pane_l);
    lv_obj_set_size(pane_l, 76, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(pane_l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane_l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pane_l, 1, 0);
    lv_obj_set_style_pad_row(pane_l, 1, 0);
    lv_obj_set_style_bg_color(pane_l, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(pane_l, LV_OPA_COVER, 0);
    s_editor_pane_l = pane_l;
    lv_obj_t *pane_r = lv_obj_create(split);
    lv_obj_remove_style_all(pane_r);
    lv_obj_set_size(pane_r, 76, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(pane_r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pane_r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pane_r, 1, 0);
    lv_obj_set_style_pad_row(pane_r, 1, 0);
    s_editor_pane_r = pane_r;
    editor_refresh_pane_l();
    editor_refresh_pane_r();
    s_editor_obj = split;
    ui_dock_create(scr, 1, 0);
}

static void editor_destroy(void)
{
    ESP_LOGI(TAG, "Editor destroy");
    s_editor_obj = NULL;
    s_editor_pane_l = NULL;
    s_editor_pane_r = NULL;
    s_editor_prog_count = 0;
    s_editor_pane = 0;
    s_editor_cat_sel = 0;
    s_editor_block_sel = 0;
    s_editor_prog_sel = 0;
    s_editor_prog_mode = 0;
    s_editor_prog_menu_sel = 0;
    s_editor_param_mode = 0;
    s_editor_run_mode = 0;
}

static bool editor_on_key(int key)
{
    if (s_editor_param_mode == 1) {
        if (key == KEY_UP) {
            s_editor_param_val++;
            if (s_editor_param_val > s_editor_param_max) s_editor_param_val = s_editor_param_max;
            char title[32];
            snprintf(title, sizeof(title), "参数: %d", s_editor_param_val);
            ui_statusbar_set_title(title);
            return true;
        }
        if (key == KEY_DOWN) {
            s_editor_param_val--;
            if (s_editor_param_val < s_editor_param_min) s_editor_param_val = s_editor_param_min;
            char title[32];
            snprintf(title, sizeof(title), "参数: %d", s_editor_param_val);
            ui_statusbar_set_title(title);
            return true;
        }
        if (key == KEY_A) {
            if (s_editor_prog_count > 0 && s_editor_prog_sel < s_editor_prog_count) {
                s_editor_prog_blocks[s_editor_prog_sel].param_val = s_editor_param_val;
            }
            s_editor_param_mode = 0;
            ui_statusbar_set_title("积木编辑器");
            editor_refresh_pane_r();
            return true;
        }
        if (key == KEY_B) {
            s_editor_param_mode = 0;
            ui_statusbar_set_title("积木编辑器");
            editor_refresh_pane_r();
            return true;
        }
        return true;
    }
    if (s_editor_prog_mode == 1) {
        if (key == KEY_UP) { s_editor_prog_menu_sel = (s_editor_prog_menu_sel - 1 + 4) % 4; editor_refresh_pane_r(); return true; }
        if (key == KEY_DOWN) { s_editor_prog_menu_sel = (s_editor_prog_menu_sel + 1) % 4; editor_refresh_pane_r(); return true; }
        if (key == KEY_A) {
            int sel = s_editor_prog_menu_sel;
            s_editor_prog_mode = 0;
            if (sel == 0) {  // 运行
                editor_run_program();
            } else if (sel == 1 && s_editor_prog_count > 0 && s_editor_prog_sel < s_editor_prog_count) {  // 删除
                for (int i = s_editor_prog_sel; i < s_editor_prog_count - 1; i++)
                    s_editor_prog_blocks[i] = s_editor_prog_blocks[i + 1];
                s_editor_prog_count--;
                if (s_editor_prog_sel >= s_editor_prog_count && s_editor_prog_count > 0)
                    s_editor_prog_sel = s_editor_prog_count - 1;
            } else if (sel == 2 && s_editor_prog_count > 1 && s_editor_prog_sel > 0) {  // 上移
                prog_block_t tmp = s_editor_prog_blocks[s_editor_prog_sel];
                s_editor_prog_blocks[s_editor_prog_sel] = s_editor_prog_blocks[s_editor_prog_sel - 1];
                s_editor_prog_blocks[s_editor_prog_sel - 1] = tmp;
                s_editor_prog_sel--;
            } else if (sel == 3 && s_editor_prog_count > 1 && s_editor_prog_sel < s_editor_prog_count - 1) {  // 下移
                prog_block_t tmp = s_editor_prog_blocks[s_editor_prog_sel];
                s_editor_prog_blocks[s_editor_prog_sel] = s_editor_prog_blocks[s_editor_prog_sel + 1];
                s_editor_prog_blocks[s_editor_prog_sel + 1] = tmp;
                s_editor_prog_sel++;
            }
            editor_refresh_pane_r();
            return true;
        }
        if (key == KEY_B) { s_editor_prog_mode = 0; editor_refresh_pane_r(); return true; }
        return true;
    }
    if (key == KEY_B) { if (ui_stack_depth() > 1) ui_stack_pop(); return true; }
    if (key == KEY_LEFT) { s_editor_pane = 0; return true; }
    if (key == KEY_RIGHT) { s_editor_pane = 1; return true; }
    if (s_editor_pane == 0) {
        if (key == KEY_UP) {
            if (s_editor_block_sel > 0) s_editor_block_sel--;
            else if (s_editor_cat_sel > 0) { s_editor_cat_sel--; s_editor_block_sel = BLOCKS_PER_CAT - 1; }
            editor_refresh_pane_l(); return true;
        }
        if (key == KEY_DOWN) {
            if (s_editor_block_sel < BLOCKS_PER_CAT - 1) s_editor_block_sel++;
            else if (s_editor_cat_sel < BLOCK_CAT_COUNT - 1) { s_editor_cat_sel++; s_editor_block_sel = 0; }
            editor_refresh_pane_l(); return true;
        }
        if (key == KEY_A && s_editor_prog_count < MAX_PROG_BLOCKS) {
            int idx = s_editor_cat_sel * BLOCKS_PER_CAT + s_editor_block_sel;
            int cat = idx / BLOCKS_PER_CAT;
            int blk = idx % BLOCKS_PER_CAT;
            int param = s_block_params[cat][blk];
            int insert_pos = s_editor_prog_count;
            if (s_editor_prog_count > 0 && s_editor_prog_sel < s_editor_prog_count) {
                insert_pos = s_editor_prog_sel;
                for (int i = s_editor_prog_count; i > insert_pos; i--)
                    s_editor_prog_blocks[i] = s_editor_prog_blocks[i - 1];
            }
            s_editor_prog_blocks[insert_pos].block_idx = idx;
            s_editor_prog_blocks[insert_pos].param_val = param;
            s_editor_prog_count++;
            s_editor_prog_sel = insert_pos;
            editor_refresh_pane_r();
            return true;
        }
    } else {
        if (s_editor_prog_count == 0) return true;
        if (key == KEY_UP) { s_editor_prog_sel = (s_editor_prog_sel - 1 + s_editor_prog_count) % s_editor_prog_count; editor_refresh_pane_r(); return true; }
        if (key == KEY_DOWN) { s_editor_prog_sel = (s_editor_prog_sel + 1) % s_editor_prog_count; editor_refresh_pane_r(); return true; }
        if (key == KEY_A) { s_editor_prog_mode = 1; s_editor_prog_menu_sel = 0; editor_refresh_pane_r(); return true; }
        if (key == KEY_LEFT || key == KEY_RIGHT) {
            int idx = s_editor_prog_blocks[s_editor_prog_sel].block_idx;
            int cat = idx / BLOCKS_PER_CAT;
            int blk = idx % BLOCKS_PER_CAT;
            if (s_block_has_param[cat][blk]) {
                s_editor_param_mode = 1;
                s_editor_param_val = s_editor_prog_blocks[s_editor_prog_sel].param_val;
                if (strstr(s_block_names[cat][blk], "移动") || strstr(s_block_names[cat][blk], "转向"))
                    { s_editor_param_min = 1; s_editor_param_max = 100; }
                else if (strstr(s_block_names[cat][blk], "等待"))
                    { s_editor_param_min = 1; s_editor_param_max = 60; }
                else if (strstr(s_block_names[cat][blk], "重复"))
                    { s_editor_param_min = 1; s_editor_param_max = 100; }
                else if (strstr(s_block_names[cat][blk], "滑行"))
                    { s_editor_param_min = 1; s_editor_param_max = 10; }
                else if (strstr(s_block_names[cat][blk], "音量"))
                    { s_editor_param_min = 0; s_editor_param_max = 100; }
                else
                    { s_editor_param_min = 0; s_editor_param_max = 100; }
                char title[32];
                snprintf(title, sizeof(title), "参数: %d", s_editor_param_val);
                ui_statusbar_set_title(title);
            }
            return true;
        }
        // A键在程序区：运行程序（长按A运行）
        // 这里使用KEY_A的另一种处理：在程序区按A弹出菜单，长按运行由外部处理
    }
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_editor_callbacks = {
    .init = editor_init,
    .destroy = editor_destroy,
    .on_key = editor_on_key,
};