/**
 * @file escher/layout.h
 * @brief Escher - GUI Toolkit: Layout Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的布局管理接口。
 * 支持垂直/水平布局、网格布局等。
 * 布局管理器直接操作控件的 x/y 坐标。
 */

#ifndef ESCHER_LAYOUT_H
#define ESCHER_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "escher/widget.h"

/**
 * @brief 垂直布局（从上到下排列子控件）
 * @param container 容器控件
 * @param spacing 子控件间距
 */
void es_layout_vertical(es_widget_t *container, int spacing);

/**
 * @brief 垂直布局（自动填充剩余空间）
 * @param container 容器控件
 * @param spacing 子控件间距
 */
void es_layout_vertical_fill(es_widget_t *container, int spacing);

/**
 * @brief 水平布局（从左到右排列子控件）
 * @param container 容器控件
 * @param spacing 子控件间距
 */
void es_layout_horizontal(es_widget_t *container, int spacing);

/**
 * @brief 水平布局（自动填充剩余空间）
 * @param container 容器控件
 * @param spacing 子控件间距
 */
void es_layout_horizontal_fill(es_widget_t *container, int spacing);

/**
 * @brief 网格布局
 * @param container 容器控件
 * @param cols 列数
 * @param spacing_x 水平间距
 * @param spacing_y 垂直间距
 */
void es_layout_grid(es_widget_t *container, int cols, int spacing_x, int spacing_y);

/**
 * @brief 网格布局（自动填充容器）
 * @param container 容器控件
 * @param cols 列数
 * @param spacing_x 水平间距
 * @param spacing_y 垂直间距
 */
void es_layout_grid_fill(es_widget_t *container, int cols, int spacing_x, int spacing_y);

/**
 * @brief 在容器中居中子控件
 * @param container 容器控件
 * @param child 子控件
 */
void es_layout_center(es_widget_t *container, es_widget_t *child);

#endif /* ESCHER_LAYOUT_H */