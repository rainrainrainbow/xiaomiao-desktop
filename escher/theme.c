/**
 * @file escher/theme.c
 * @brief Escher - GUI Toolkit: Theme Implementation
 *
 * 主题系统实现：Light（浅色）、Dark（深色）、Metro（WP8 风格）。
 * 支持主题切换和颜色管理。
 */

#include "escher/theme.h"
#include "kandinsky/canvas.h"
#include <string.h>

/* ========== 主题状态 ========== */
static es_theme_t s_current_theme;
static es_theme_type_t s_current_theme_type = ES_THEME_METRO;
static bool s_theme_initialized = false;

/* ========== 主题定义 ========== */

/* Metro 主题（WP8 风格） */
static const es_theme_t s_theme_metro = {
    .type = ES_THEME_METRO,
    .name = "Metro",
    .colors = {
        .background      = KD_COLOR_FROM_RGB(0x1A, 0x1A, 0x2E),  /* 深蓝黑 */
        .surface         = KD_COLOR_FROM_RGB(0x16, 0x21, 0x3E),  /* 深蓝 */
        .primary         = KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),  /* WP8 蓝 */
        .secondary       = KD_COLOR_FROM_RGB(0x00, 0x50, 0x8F),  /* 深蓝 */
        .accent          = KD_COLOR_FROM_RGB(0xE8, 0x48, 0x10),  /* 橙红 */
        .text_primary    = KD_COLOR_FROM_RGB(0xFF, 0xFF, 0xFF),  /* 白色 */
        .text_secondary  = KD_COLOR_FROM_RGB(0xAA, 0xAA, 0xAA),  /* 灰色 */
        .text_disabled   = KD_COLOR_FROM_RGB(0x55, 0x55, 0x55),  /* 暗灰 */
        .border          = KD_COLOR_FROM_RGB(0x33, 0x33, 0x55),  /* 边框 */
        .divider         = KD_COLOR_FROM_RGB(0x2A, 0x2A, 0x4A),  /* 分割线 */
        .success         = KD_COLOR_FROM_RGB(0x10, 0xB9, 0x81),  /* 绿色 */
        .warning         = KD_COLOR_FROM_RGB(0xFF, 0xA0, 0x00),  /* 橙色 */
        .error           = KD_COLOR_FROM_RGB(0xE8, 0x48, 0x10),  /* 红色 */
        .highlight       = KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),  /* 高亮 */
        .transparent     = 0xFFFF,                                 /* 透明色标记 */
    },
    .dock_height = 8,
    .title_height = 12,
    .icon_size = 32,
    .spacing = 4,
    .padding = 4,
    .radius = 4,
};

/* 浅色主题 */
static const es_theme_t s_theme_light = {
    .type = ES_THEME_LIGHT,
    .name = "Light",
    .colors = {
        .background      = KD_COLOR_FROM_RGB(0xF5, 0xF5, 0xF5),  /* 浅灰 */
        .surface         = KD_COLOR_WHITE,
        .primary         = KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),  /* WP8 蓝 */
        .secondary       = KD_COLOR_FROM_RGB(0x00, 0x50, 0x8F),
        .accent          = KD_COLOR_FROM_RGB(0xE8, 0x48, 0x10),
        .text_primary    = KD_COLOR_FROM_RGB(0x22, 0x22, 0x22),  /* 深灰 */
        .text_secondary  = KD_COLOR_FROM_RGB(0x66, 0x66, 0x66),
        .text_disabled   = KD_COLOR_FROM_RGB(0xAA, 0xAA, 0xAA),
        .border          = KD_COLOR_FROM_RGB(0xDD, 0xDD, 0xDD),
        .divider         = KD_COLOR_FROM_RGB(0xEE, 0xEE, 0xEE),
        .success         = KD_COLOR_FROM_RGB(0x10, 0xB9, 0x81),
        .warning         = KD_COLOR_FROM_RGB(0xFF, 0xA0, 0x00),
        .error           = KD_COLOR_FROM_RGB(0xE8, 0x48, 0x10),
        .highlight       = KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),
        .transparent     = 0xFFFF,
    },
    .dock_height = 8,
    .title_height = 12,
    .icon_size = 32,
    .spacing = 4,
    .padding = 4,
    .radius = 4,
};

/* 深色主题 */
static const es_theme_t s_theme_dark = {
    .type = ES_THEME_DARK,
    .name = "Dark",
    .colors = {
        .background      = KD_COLOR_FROM_RGB(0x12, 0x12, 0x12),
        .surface         = KD_COLOR_FROM_RGB(0x1E, 0x1E, 0x1E),
        .primary         = KD_COLOR_FROM_RGB(0xBB, 0x86, 0xFC),
        .secondary       = KD_COLOR_FROM_RGB(0x7C, 0x4D, 0xFF),
        .accent          = KD_COLOR_FROM_RGB(0x03, 0xD4, 0xAC),
        .text_primary    = KD_COLOR_FROM_RGB(0xE0, 0xE0, 0xE0),
        .text_secondary  = KD_COLOR_FROM_RGB(0x80, 0x80, 0x80),
        .text_disabled   = KD_COLOR_FROM_RGB(0x40, 0x40, 0x40),
        .border          = KD_COLOR_FROM_RGB(0x33, 0x33, 0x33),
        .divider         = KD_COLOR_FROM_RGB(0x2A, 0x2A, 0x2A),
        .success         = KD_COLOR_FROM_RGB(0x10, 0xB9, 0x81),
        .warning         = KD_COLOR_FROM_RGB(0xFF, 0xA0, 0x00),
        .error           = KD_COLOR_FROM_RGB(0xCF, 0x66, 0x78),
        .highlight       = KD_COLOR_FROM_RGB(0xBB, 0x86, 0xFC),
        .transparent     = 0xFFFF,
    },
    .dock_height = 8,
    .title_height = 12,
    .icon_size = 32,
    .spacing = 4,
    .padding = 4,
    .radius = 4,
};

/* ========== 主题 API ========== */

void es_theme_init(es_theme_type_t type)
{
    s_current_theme_type = type;
    es_theme_apply(type);
    s_theme_initialized = true;
}

void es_theme_apply(es_theme_type_t type)
{
    switch (type) {
        case ES_THEME_LIGHT:
            memcpy(&s_current_theme, &s_theme_light, sizeof(es_theme_t));
            break;
        case ES_THEME_DARK:
            memcpy(&s_current_theme, &s_theme_dark, sizeof(es_theme_t));
            break;
        case ES_THEME_METRO:
        default:
            memcpy(&s_current_theme, &s_theme_metro, sizeof(es_theme_t));
            break;
    }
    s_current_theme_type = type;
}

const es_theme_t *es_theme_get_current(void)
{
    if (!s_theme_initialized) {
        es_theme_init(ES_THEME_METRO);
    }
    return &s_current_theme;
}

es_theme_type_t es_theme_get_type(void)
{
    return s_current_theme_type;
}

es_theme_type_t es_theme_next(void)
{
    es_theme_type_t next = (s_current_theme_type + 1) % 3;
    es_theme_apply(next);
    return next;
}

const es_theme_t *es_theme_get(es_theme_type_t type)
{
    switch (type) {
        case ES_THEME_LIGHT: return &s_theme_light;
        case ES_THEME_DARK:  return &s_theme_dark;
        case ES_THEME_METRO:
        default:             return &s_theme_metro;
    }
}

/* ========== 颜色获取 ========== */

ion_color_t es_color_get(es_color_role_t role)
{
    const es_theme_t *theme = es_theme_get_current();
    if (!theme) return KD_COLOR_BLACK;

    switch (role) {
        case ES_COLOR_BACKGROUND:     return theme->colors.background;
        case ES_COLOR_SURFACE:        return theme->colors.surface;
        case ES_COLOR_PRIMARY:        return theme->colors.primary;
        case ES_COLOR_SECONDARY:      return theme->colors.secondary;
        case ES_COLOR_ACCENT:         return theme->colors.accent;
        case ES_COLOR_TEXT_PRIMARY:   return theme->colors.text_primary;
        case ES_COLOR_TEXT_SECONDARY: return theme->colors.text_secondary;
        case ES_COLOR_TEXT_DISABLED:  return theme->colors.text_disabled;
        case ES_COLOR_BORDER:         return theme->colors.border;
        case ES_COLOR_DIVIDER:        return theme->colors.divider;
        case ES_COLOR_SUCCESS:        return theme->colors.success;
        case ES_COLOR_WARNING:        return theme->colors.warning;
        case ES_COLOR_ERROR:          return theme->colors.error;
        case ES_COLOR_HIGHLIGHT:      return theme->colors.highlight;
        case ES_COLOR_TRANSPARENT:    return theme->colors.transparent;
        default:                      return KD_COLOR_BLACK;
    }
}