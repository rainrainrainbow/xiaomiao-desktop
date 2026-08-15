/**
 * @file escher/theme.h
 * @brief Escher - GUI Toolkit: Theme Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的主题系统。
 * 支持 Light（浅色）、Dark（深色）、Metro（WP8 风格）三种主题。
 * 通过 es_color_get() 获取指定角色的颜色值。
 */

#ifndef THEME_H
#define THEME_H

#include <stdint.h>
#include <stdbool.h>
#include "kandinsky/canvas.h"

/* 主题类型枚举 */
typedef enum {
    ES_THEME_LIGHT = 0,
    ES_THEME_DARK,
    ES_THEME_METRO,
} es_theme_type_t;

/* 颜色角色枚举 */
typedef enum {
    ES_COLOR_BACKGROUND = 0,
    ES_COLOR_SURFACE,
    ES_COLOR_PRIMARY,
    ES_COLOR_SECONDARY,
    ES_COLOR_ACCENT,
    ES_COLOR_TEXT_PRIMARY,
    ES_COLOR_TEXT_SECONDARY,
    ES_COLOR_TEXT_DISABLED,
    ES_COLOR_BORDER,
    ES_COLOR_DIVIDER,
    ES_COLOR_SUCCESS,
    ES_COLOR_WARNING,
    ES_COLOR_ERROR,
    ES_COLOR_HIGHLIGHT,
    ES_COLOR_TRANSPARENT,
} es_color_role_t;

/* 主题颜色结构体 */
typedef struct {
    ion_color_t background;
    ion_color_t surface;
    ion_color_t primary;
    ion_color_t secondary;
    ion_color_t accent;
    ion_color_t text_primary;
    ion_color_t text_secondary;
    ion_color_t text_disabled;
    ion_color_t border;
    ion_color_t divider;
    ion_color_t success;
    ion_color_t warning;
    ion_color_t error;
    ion_color_t highlight;
    ion_color_t transparent;
} es_theme_colors_t;

/* 主题结构体 */
typedef struct {
    es_theme_type_t type;
    const char *name;
    es_theme_colors_t colors;
    int dock_height;
    int title_height;
    int icon_size;
    int spacing;
    int padding;
    int radius;
} es_theme_t;

/**
 * @brief 初始化主题系统
 * @param type 初始主题类型
 */
void es_theme_init(es_theme_type_t type);

/**
 * @brief 应用指定主题
 * @param type 主题类型
 */
void es_theme_apply(es_theme_type_t type);

/**
 * @brief 获取当前主题
 * @return 主题指针
 */
const es_theme_t *es_theme_get_current(void);

/**
 * @brief 获取当前主题类型
 * @return 主题类型
 */
es_theme_type_t es_theme_get_type(void);

/**
 * @brief 切换到下一个主题
 * @return 新的主题类型
 */
es_theme_type_t es_theme_next(void);

/**
 * @brief 获取指定主题
 * @param type 主题类型
 * @return 主题指针
 */
const es_theme_t *es_theme_get(es_theme_type_t type);

/**
 * @brief 获取指定角色的颜色值
 * @param role 颜色角色
 * @return RGB565 颜色值
 */
ion_color_t es_color_get(es_color_role_t role);

#endif /* THEME_H */