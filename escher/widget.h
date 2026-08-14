/**
 * @file escher/widget.h
 * @brief Escher - GUI Toolkit: Widget Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的控件接口。
 * 基于 Kandinsky 图形引擎，提供高级 UI 组件。
 * 控件树结构：Container 包含子控件，递归绘制和事件处理。
 */

#ifndef ESCHER_WIDGET_H
#define ESCHER_WIDGET_H

#include <stdint.h>
#include <stdbool.h>
#include "kandinsky/canvas.h"
#include "kandinsky/font.h"

/* 最大子控件数量 */
#define ES_MAX_CHILDREN 16

/* 控件类型枚举 */
typedef enum {
    ES_WIDGET_LABEL = 0,
    ES_WIDGET_BUTTON,
    ES_WIDGET_CONTAINER,
} es_widget_type_t;

/* 文本对齐方式 */
typedef enum {
    ES_ALIGN_LEFT = 0,
    ES_ALIGN_CENTER,
    ES_ALIGN_RIGHT,
} es_align_t;

/* 事件类型 */
typedef enum {
    ES_EVENT_KEY_DOWN = 0,
    ES_EVENT_KEY_UP,
    ES_EVENT_KEY_LONG_PRESS,
    ES_EVENT_TOUCH_DOWN,
    ES_EVENT_TOUCH_UP,
    ES_EVENT_TOUCH_MOVE,
} es_event_type_t;

/* 按键枚举 */
typedef enum {
    ES_KEY_UP = 0,
    ES_KEY_DOWN,
    ES_KEY_LEFT,
    ES_KEY_RIGHT,
    ES_KEY_A,
    ES_KEY_B,
} es_key_t;

/* 事件结构体 */
typedef struct {
    es_event_type_t type;
    union {
        es_key_t key;
        struct {
            int x, y;
            void *target;
        } touch;
    };
} es_event_t;

/* 前向声明 */
typedef struct es_widget_s es_widget_t;

/* 事件回调函数类型 */
typedef bool (*es_widget_event_cb)(es_widget_t *widget, es_event_t *event);

/* 控件结构体 */
struct es_widget_s {
    int id;                          /* 唯一 ID */
    es_widget_type_t type;           /* 控件类型 */
    int x, y, width, height;        /* 位置和大小 */
    bool visible;                    /* 可见性 */
    bool enabled;                    /* 可用性 */
    
    /* 颜色 */
    ion_color_t bg_color;           /* 背景色 */
    ion_color_t fg_color;           /* 前景色（文字色） */
    ion_color_t border_color;       /* 边框色 */
    int border_width;               /* 边框宽度 */
    int radius;                     /* 圆角半径 */
    
    /* 文本 */
    const char *text;               /* 文本内容 */
    const kd_font_t *font;          /* 字体 */
    es_align_t align;               /* 对齐方式 */
    
    /* 标签 */
    const char *tag;                /* 用于查找的标签 */
    
    /* 回调 */
    es_widget_event_cb on_event;    /* 事件处理回调 */
    
    /* 控件树 */
    es_widget_t *parent;            /* 父控件 */
    es_widget_t *children[ES_MAX_CHILDREN]; /* 子控件 */
    int child_count;                /* 子控件数量 */
};

/**
 * @brief 初始化控件
 */
void es_widget_init(es_widget_t *widget, es_widget_type_t type);

/**
 * @brief 设置根控件
 */
void es_widget_set_root(es_widget_t *root);

/**
 * @brief 获取根控件
 */
es_widget_t *es_widget_get_root(void);

/**
 * @brief 添加子控件
 */
void es_widget_add_child(es_widget_t *parent, es_widget_t *child);

/**
 * @brief 移除子控件
 */
void es_widget_remove_child(es_widget_t *parent, es_widget_t *child);

/**
 * @brief 移除所有子控件
 */
void es_widget_remove_all_children(es_widget_t *parent);

/**
 * @brief 设置控件位置和大小
 */
void es_widget_set_frame(es_widget_t *widget, int x, int y, int w, int h);

/**
 * @brief 设置控件位置
 */
void es_widget_set_pos(es_widget_t *widget, int x, int y);

/**
 * @brief 设置控件大小
 */
void es_widget_set_size(es_widget_t *widget, int w, int h);

/**
 * @brief 获取控件绝对位置
 */
void es_widget_absolute_pos(es_widget_t *widget, int *out_x, int *out_y);

/**
 * @brief 设置可见性
 */
void es_widget_set_visible(es_widget_t *widget, bool visible);

/**
 * @brief 设置可用性
 */
void es_widget_set_enabled(es_widget_t *widget, bool enabled);

/**
 * @brief 设置焦点控件
 */
void es_widget_set_focus(es_widget_t *widget);

/**
 * @brief 获取焦点控件
 */
es_widget_t *es_widget_get_focus(void);

/**
 * @brief 递归绘制控件树
 */
void es_widget_draw(es_widget_t *widget);

/**
 * @brief 绘制整个控件树
 */
void es_widget_draw_all(void);

/**
 * @brief 处理控件事件
 */
bool es_widget_handle_event(es_widget_t *widget, es_event_t *event);

/**
 * @brief 处理触摸事件
 */
bool es_widget_handle_touch(int px, int py, es_event_type_t type);

/**
 * @brief 处理按键事件
 */
bool es_widget_handle_key(es_key_t key, es_event_type_t type);

/**
 * @brief 按 ID 查找控件
 */
es_widget_t *es_widget_find_by_id(es_widget_t *root, int id);

/**
 * @brief 按标签查找控件
 */
es_widget_t *es_widget_find_by_tag(es_widget_t *root, const char *tag);

/**
 * @brief 创建标签控件
 */
es_widget_t *es_label_create(int x, int y, int w, int h, const char *text);

/**
 * @brief 创建按钮控件
 */
es_widget_t *es_button_create(int x, int y, int w, int h, const char *text);

/**
 * @brief 创建容器控件
 */
es_widget_t *es_container_create(int x, int y, int w, int h);

#endif /* ESCHER_WIDGET_H */