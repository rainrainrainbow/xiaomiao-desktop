/**
 * @file ui_keyboard.h
 * @brief 虚拟键盘组件 - 紧凑全键盘输入（适配160x128小屏+6键导航）
 *
 * 布局：
 *   顶部：输入框（显示已输入文本）
 *   中间：字符网格（5行×7列=35字符位，含模式切换）
 *   底部：操作栏（模式/空格/退格/确认）
 *
 * 操作：
 *   UP/DOWN — 移动行选择
 *   LEFT/RIGHT — 移动列选择
 *   A — 输入选中字符 / 确认
 *   B — 退格 / 返回
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
