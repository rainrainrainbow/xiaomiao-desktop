/**
 * @file ui_keyboard.h
 * @brief 虚拟键盘组件 - 5 键优先输入（适配 160x128 小屏 + 四向键 + A/B）
 *
 * 布局：
 *   顶部：标题（可选）+ 输入框
 *   中部：字符网格（默认 T9 九宫格 3x3 大键；可切换 QWERTY 6x5）
 *   底部：操作栏（布局 T9/QW、模式 abc/ABC/123、A=输入 B=退格 提示）
 *
 * 5 键操作：
 *   UP/DOWN/LEFT/RIGHT — 在网格中移动选中项
 *   A — 输入字符；T9 下连按 A 在同一字母组内循环（多输），方向键/空格/确认敲定
 *   B — 退格（有文本时），无文本时取消（回调 on_cancel）
 *
 * 说明：键盘输出字符（ASCII 拼音/英文/数字/符号），中文候选词不在本组件范围内。
 */

#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 键盘模式 */
typedef enum {
    KB_MODE_LOWER = 0,   /* 小写字母 */
    KB_MODE_UPPER,       /* 大写字母 */
    KB_MODE_NUMBER,      /* 数字+符号 */
    KB_MODE_SYMBOL,      /* 特殊符号 */
    KB_MODE_MAX
} kb_mode_t;

/* 键盘布局 */
typedef enum {
    KB_LAYOUT_QWERTY = 0,    /* QWERTY 全键盘（手机全键盘风格） */
    KB_LAYOUT_T9,            /* T9 九宫格（老式手机拨号盘） */
    KB_LAYOUT_MAX
} kb_layout_t;

/* 键盘回调 */
typedef void (*kb_confirm_cb_t)(const char *text, void *user_data);
typedef void (*kb_cancel_cb_t)(void *user_data);

/* 键盘配置 */
typedef struct {
    const char *title;          /* 标题（如"WiFi密码"） */
    const char *placeholder;    /* 占位符文本 */
    int max_length;             /* 最大输入长度（0=不限制，默认64） */
    bool password_mode;         /* 密码模式（显示*号） */
    kb_confirm_cb_t on_confirm; /* 确认回调 */
    kb_cancel_cb_t on_cancel;   /* 取消回调 */
    void *user_data;            /* 用户数据 */
} kb_config_t;

/**
 * 显示虚拟键盘（全屏覆盖）
 * @param config 键盘配置
 */
void ui_keyboard_show(const kb_config_t *config);

/**
 * 隐藏并销毁键盘
 */
void ui_keyboard_hide(void);

/**
 * 键盘是否正在显示
 */
bool ui_keyboard_is_visible(void);

/**
 * 键盘按键处理（由主循环转发）
 * @param key 按键
 * @return true 已处理
 */
bool ui_keyboard_on_key(int key);

/**
 * 获取当前输入文本
 */
const char* ui_keyboard_get_text(void);

/**
 * 设置输入文本
 */
void ui_keyboard_set_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* UI_KEYBOARD_H */
