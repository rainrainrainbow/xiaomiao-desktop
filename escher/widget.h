/**
 * @file escher/widget.h
 * @brief Escher - GUI Toolkit: Widget Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的控件接口。
 * 基于 Kandinsky 图形引擎，提供高级 UI 组件。
 */

#ifndef ESCHER_WIDGET_H
#define ESCHER_WIDGET_H

#include <stdint.h>
#include <stdbool.h>
#include "kandinsky/canvas.h"

/* 控件类型枚举 */
typedef enum {
    ESCHER_WIDGET_LABEL = 0,
    ESCHER_WIDGET_BUTTON,
    ESCHER_WIDGET_CONTAINER,
    ESCHER_WIDGET_LIST,
    ESCHER_WIDGET_ICON
} escher_widget_type_t;

/* 控件结构体（前向声明） */
typedef struct escher_widget_t escher_widget_t;

/* 控件回调函数类型 */
typedef void (*escher_widget_draw_cb)(escher_widget_t *widget, kandinsky_canvas_t *canvas);
typedef bool (*escher_widget_key_cb)(escher_widget_t *widget, int key);

/**
 * @brief 创建标签控件
 * @param text 文本内容（UTF-8）
 * @param x X 坐标
 * @param y Y 坐标
 * @return 控件指针，NULL 表示失败
 */
escher_widget_t* escher_widget_create_label(const char *text, int x, int y);

/**
 * @brief 创建按钮控件
 * @param text 按钮文本
 * @param x X 坐标
 * @param y Y 坐标
 * @param width 宽度
 * @param height 高度
 * @return 控件指针，NULL 表示失败
 */
escher_widget_t* escher_widget_create_button(const char *text, int x, int y, int width, int height);

/**
 * @brief 创建容器控件（用于布局）
 * @param x X 坐标
 * @param y Y 坐标
 * @param width 宽度
 * @param height 高度
 * @return 控件指针，NULL 表示失败
 */
escher_widget_t* escher_widget_create_container(int x, int y, int width, int height);

/**
 * @brief 销毁控件
 * @param widget 控件指针
 */
void escher_widget_destroy(escher_widget_t *widget);

/**
 * @brief 绘制控件
 * @param widget 控件指针
 * @param canvas 画布指针
 */
void escher_widget_draw(escher_widget_t *widget, kandinsky_canvas_t *canvas);

/**
 * @brief 处理按键事件
 * @param widget 控件指针
 * @param key 按键枚举
 * @return true 已处理，false 未处理
 */
bool escher_widget_handle_key(escher_widget_t *widget, int key);

/**
 * @brief 设置控件可见性
 * @param widget 控件指针
 * @param visible true 可见，false 隐藏
 */
void escher_widget_set_visible(escher_widget_t *widget, bool visible);

/**
 * @brief 检查控件是否可见
 * @param widget 控件指针
 * @return true 可见，false 隐藏
 */
bool escher_widget_is_visible(escher_widget_t *widget);

#endif /* ESCHER_WIDGET_H */