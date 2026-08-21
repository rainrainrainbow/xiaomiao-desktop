/**
 * @file escher/layout.c
 * @brief Escher - GUI Toolkit: Layout Implementation
 *
 * 布局管理器实现：Vertical（垂直）、Horizontal（水平）、Grid（网格）。
 * 采用 NumWorks Epsilon 风格：自动排列子控件位置。
 */

#include "escher/layout.h"
#include <string.h>

/* ========== 垂直布局 ========== */

void es_layout_vertical(es_widget_t *container, int spacing)
{
    if (!container) return;

    int y = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->y = y;
        y += child->height + spacing;
    }
}

void es_layout_vertical_fill(es_widget_t *container, int spacing)
{
    if (!container || container->child_count == 0) return;

    /* 计算可见子控件数量 */
    int visible_count = 0;
    int total_fixed = 0;
    for (int i = 0; i < container->child_count; i++) {
        if (!container->children[i]->visible) continue;
        visible_count++;
        if (container->children[i]->height > 0) {
            total_fixed += container->children[i]->height;
        }
    }

    if (visible_count == 0) return;

    int total_spacing = (visible_count - 1) * spacing;
    int remaining = container->height - total_fixed - total_spacing;
    int fill_count = 0;

    /* 计算需要填充的控件数 */
    for (int i = 0; i < container->child_count; i++) {
        if (container->children[i]->visible && container->children[i]->height <= 0) {
            fill_count++;
        }
    }

    int fill_height = (fill_count > 0) ? (remaining / fill_count) : 0;

    int y = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->y = y;
        if (child->height <= 0) {
            child->height = fill_height;
        }
        y += child->height + spacing;
    }
}

/* ========== 水平布局 ========== */

void es_layout_horizontal(es_widget_t *container, int spacing)
{
    if (!container) return;

    int x = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->x = x;
        x += child->width + spacing;
    }
}

void es_layout_horizontal_fill(es_widget_t *container, int spacing)
{
    if (!container || container->child_count == 0) return;

    /* 计算可见子控件数量 */
    int visible_count = 0;
    int total_fixed = 0;
    for (int i = 0; i < container->child_count; i++) {
        if (!container->children[i]->visible) continue;
        visible_count++;
        if (container->children[i]->width > 0) {
            total_fixed += container->children[i]->width;
        }
    }

    if (visible_count == 0) return;

    int total_spacing = (visible_count - 1) * spacing;
    int remaining = container->width - total_fixed - total_spacing;
    int fill_count = 0;

    for (int i = 0; i < container->child_count; i++) {
        if (container->children[i]->visible && container->children[i]->width <= 0) {
            fill_count++;
        }
    }

    int fill_width = (fill_count > 0) ? (remaining / fill_count) : 0;

    int x = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->x = x;
        if (child->width <= 0) {
            child->width = fill_width;
        }
        x += child->width + spacing;
    }
}

/* ========== 网格布局 ========== */

void es_layout_grid(es_widget_t *container, int cols, int spacing_x, int spacing_y)
{
    if (!container || cols <= 0) return;

    int row = 0, col = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->x = col * (child->width + spacing_x);
        child->y = row * (child->height + spacing_y);

        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }
}

void es_layout_grid_fill(es_widget_t *container, int cols, int spacing_x, int spacing_y)
{
    if (!container || cols <= 0) return;

    int cell_w = (container->width - (cols - 1) * spacing_x) / cols;
    int rows = (container->child_count + cols - 1) / cols;
    int cell_h = (container->height - (rows - 1) * spacing_y) / rows;

    int row = 0, col = 0;
    for (int i = 0; i < container->child_count; i++) {
        es_widget_t *child = container->children[i];
        if (!child->visible) continue;

        child->x = col * (cell_w + spacing_x);
        child->y = row * (cell_h + spacing_y);
        child->width = cell_w;
        child->height = cell_h;

        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }
}

/* ========== 居中对齐 ========== */

void es_layout_center(es_widget_t *container, es_widget_t *child)
{
    if (!container || !child) return;

    child->x = (container->width - child->width) / 2;
    child->y = (container->height - child->height) / 2;
}