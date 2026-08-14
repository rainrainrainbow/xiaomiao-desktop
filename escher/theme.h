/**
 * @file escher/theme.h
 * @brief Escher - GUI Toolkit: Theme Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的主题系统。
 * 支持浅色/深色主题切换。
 */

#ifndef ESCHER_THEME_H
#define ESCHER_THEME_H

#include <stdint.h>
#include "ion/display.h"

/* 主题类型枚举 */
typedef enum {
    ESCHER_THEME_LIGHT = 0,
    ESCHER_THEME_DARK
} escher_theme_type_t;

/* 主题颜色结构体 */
typedef struct {
    ion_color_t bg;           /* 背景色 */
    ion_color_t text;         /* 文本色 */
    ion_color_t primary;      /* 主色调 */
    ion_color_t secondary;    /* 次色调 */
    ion_color_t accent;       /* 强调色 */
    ion_color_t border;       /* 边框色 */
    ion_color_t disabled;     /* 禁用状态色 */
} escher_theme_colors_t;

/**
 * @brief 初始化主题系统
 * @return true 成功，false 失败
 */
bool escher_theme_init(void);

/**
 * @brief 设置当前主题
 * @param theme 主题类型
 */
void escher_theme_set(escher_theme_type_t theme);

/**
 * @brief 获取当前主题类型
 * @return 主题类型
 */
escher_theme_type_t escher_theme_get(void);

/**
 * @brief 获取当前主题的颜色配置
 * @return 主题颜色结构体指针
 */
const escher_theme_colors_t* escher_theme_get_colors(void);

/**
 * @brief 获取 Metro UI 风格的主题颜色（Windows Phone 风格）
 * @return 主题颜色结构体指针
 */
const escher_theme_colors_t* escher_theme_get_metro(void);

#endif /* ESCHER_THEME_H */