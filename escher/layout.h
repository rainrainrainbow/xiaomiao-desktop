/**
 * @file escher/layout.h
 * @brief Escher - GUI Toolkit: Layout Interface
 * 
 * 参考 NumWorks Epsilon 的 Escher 层设计，提供统一的布局管理接口。
 * 支持垂直/水平布局、网格布局等。
 */

#ifndef ESCHER_LAYOUT_H
#define ESCHER_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "escher/widget.h"

/* 布局类型枚举 */
typedef enum {
    ESCHER_LAYOUT_VERTICAL = 0,   /* 垂直布局 */
    ESCHER_LAYOUT_HORIZONTAL,     /* 水平布局 */
    ESCHER_LAYOUT_GRID            /* 网格布局 */
} escher_layout_type_t;

/* 布局结构体（前向声明） */
typedef struct escher_layout_t escher_layout_t;

/**
 * @brief 创建布局
 * @param type 布局类型
 * @param x X 坐标
 * @param y Y 坐标
 * @param width 宽度
 * @param height 高度
 * @return 布局指针，NULL 表示失败
 */
escher_layout_t* escher_layout_create(escher_layout_type_t type, int x, int y, int width, int height);

/**
 * @brief 销毁布局
 * @param layout 布局指针
 */
void escher_layout_destroy(escher_layout_t *layout);

/**
 * @brief 向布局添加子控件
 * @param layout 布局指针
 * @param widget 子控件指针
 * @return true 成功，false 失败
 */
bool escher_layout_add_child(escher_layout_t *layout, escher_widget_t *widget);

/**
 * @brief 从布局移除子控件
 * @param layout 布局指针
 * @param widget 子控件指针
 * @return true 成功，false 失败
 */
bool escher_layout_remove_child(escher_layout_t *layout, escher_widget_t *widget);

/**
 * @brief 设置网格布局的列数
 * @param layout 布局指针（必须是 GRID 类型）
 * @param columns 列数
 */
void escher_layout_set_grid_columns(escher_layout_t *layout, int columns);

/**
 * @brief 设置子控件间距
 * @param layout 布局指针
 * @param spacing 间距（像素）
 */
void escher_layout_set_spacing(escher_layout_t *layout, int spacing);

/**
 * @brief 计算并应用布局
 * @param layout 布局指针
 */
void escher_layout_apply(escher_layout_t *layout);

#endif /* ESCHER_LAYOUT_H */