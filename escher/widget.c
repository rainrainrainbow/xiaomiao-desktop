/**
 * @file escher/widget.c
 * @brief Escher - GUI Toolkit: Widget Implementation
 *
 * 基础控件实现：Label（标签）、Button（按钮）、Container（容器）。
 * 采用 NumWorks Epsilon 风格：控件树结构，统一绘制/事件接口。
 */

#include "escher/widget.h"
#include "escher/theme.h"
#include "kandinsky/canvas.h"
#include "kandinsky/font.h"
#include <string.h>
#include <stdlib.h>

/* ========== 控件树管理 ========== */

/* 根控件 */
static es_widget_t *s_root = NULL;

/* 当前焦点控件 */
static es_widget_t *s_focus = NULL;

/* 点击事件目标 */
static es_widget_t *s_touch_target = NULL;

/* 控件 ID 计数器 */
static int s_next_id = 1;

void es_widget_init(es_widget_t *widget, es_widget_type_t type)
{
    if (!widget) return;
    memset(widget, 0, sizeof(es_widget_t));
    widget->type = type;
    widget->id = s_next_id++;
    widget->visible = true;
    widget->enabled = true;
    widget->bg_color = ES_COLOR_TRANSPARENT;
    widget->fg_color = ES_COLOR_TEXT_PRIMARY;
    widget->border_color = ES_COLOR_TRANSPARENT;
    widget->border_width = 0;
    widget->radius = 0;
}

void es_widget_set_root(es_widget_t *root)
{
    s_root = root;
}

es_widget_t *es_widget_get_root(void)
{
    return s_root;
}

/* ========== 子控件管理 ========== */

void es_widget_add_child(es_widget_t *parent, es_widget_t *child)
{
    if (!parent || !child) return;
    if (parent->child_count >= ES_MAX_CHILDREN) return;

    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

void es_widget_remove_child(es_widget_t *parent, es_widget_t *child)
{
    if (!parent || !child) return;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            child->parent = NULL;
            /* 移除子控件 */
            for (int j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->children[--parent->child_count] = NULL;
            return;
        }
    }
}

void es_widget_remove_all_children(es_widget_t *parent)
{
    if (!parent) return;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i]) {
            parent->children[i]->parent = NULL;
        }
        parent->children[i] = NULL;
    }
    parent->child_count = 0;
}

/* ========== 位置和大小 ========== */

void es_widget_set_frame(es_widget_t *widget, int x, int y, int w, int h)
{
    if (!widget) return;
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;
}

void es_widget_set_pos(es_widget_t *widget, int x, int y)
{
    if (!widget) return;
    widget->x = x;
    widget->y = y;
}

void es_widget_set_size(es_widget_t *widget, int w, int h)
{
    if (!widget) return;
    widget->width = w;
    widget->height = h;
}

/* 计算控件在屏幕上的绝对位置 */
void es_widget_absolute_pos(es_widget_t *widget, int *out_x, int *out_y)
{
    int x = 0, y = 0;
    es_widget_t *w = widget;
    while (w) {
        x += w->x;
        y += w->y;
        w = w->parent;
    }
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}

/* ========== 可见性和启用 ========== */

void es_widget_set_visible(es_widget_t *widget, bool visible)
{
    if (widget) widget->visible = visible;
}

void es_widget_set_enabled(es_widget_t *widget, bool enabled)
{
    if (widget) widget->enabled = enabled;
}

/* ========== 焦点管理 ========== */

void es_widget_set_focus(es_widget_t *widget)
{
    s_focus = widget;
}

es_widget_t *es_widget_get_focus(void)
{
    return s_focus;
}

/* ========== 绘制 ========== */

/* 绘制控件背景（含圆角） */
static void draw_background(es_widget_t *widget, int abs_x, int abs_y)
{
    if (widget->bg_color == ES_COLOR_TRANSPARENT) return;

    if (widget->radius > 0) {
        kd_canvas_fill_round_rect(abs_x, abs_y, widget->width, widget->height,
                                   widget->radius, widget->bg_color);
    } else {
        kd_canvas_fill_rect(abs_x, abs_y, widget->width, widget->height,
                             widget->bg_color);
    }
}

/* 绘制控件边框 */
static void draw_border(es_widget_t *widget, int abs_x, int abs_y)
{
    if (widget->border_width <= 0 || widget->border_color == ES_COLOR_TRANSPARENT) return;

    kd_canvas_draw_rect(abs_x, abs_y, widget->width, widget->height, widget->border_color);

    /* 如果边框宽度 > 1，绘制多层 */
    for (int i = 1; i < widget->border_width && i < 4; i++) {
        kd_canvas_draw_rect(abs_x + i, abs_y + i,
                             widget->width - 2 * i, widget->height - 2 * i,
                             widget->border_color);
    }
}

/* 绘制 Label 控件 */
static void draw_label(es_widget_t *widget, int abs_x, int abs_y)
{
    draw_background(widget, abs_x, abs_y);
    draw_border(widget, abs_x, abs_y);

    if (widget->text && widget->text[0]) {
        const kd_font_t *font = widget->font ? widget->font : kd_font_get_default();
        int fh = kd_font_height(font);
        int tw = kd_font_string_width(font, widget->text);

        /* 文本居中 */
        int tx = abs_x + (widget->width - tw) / 2;
        int ty = abs_y + (widget->height - fh) / 2;

        if (widget->align == ES_ALIGN_LEFT) {
            tx = abs_x + 4;
        } else if (widget->align == ES_ALIGN_RIGHT) {
            tx = abs_x + widget->width - tw - 4;
        }

        kd_font_draw_string(tx, ty, font, widget->text, widget->fg_color, ES_COLOR_TRANSPARENT);
    }
}

/* 绘制 Button 控件 */
static void draw_button(es_widget_t *widget, int abs_x, int abs_y)
{
    draw_background(widget, abs_x, abs_y);
    draw_border(widget, abs_x, abs_y);

    if (widget->text && widget->text[0]) {
        const kd_font_t *font = widget->font ? widget->font : kd_font_get_default();
        int fh = kd_font_height(font);
        int tw = kd_font_string_width(font, widget->text);

        /* 文本居中 */
        int tx = abs_x + (widget->width - tw) / 2;
        int ty = abs_y + (widget->height - fh) / 2;

        kd_font_draw_string(tx, ty, font, widget->text, widget->fg_color, ES_COLOR_TRANSPARENT);
    }
}

/* 绘制 Container 控件 */
static void draw_container(es_widget_t *widget, int abs_x, int abs_y)
{
    draw_background(widget, abs_x, abs_y);
    draw_border(widget, abs_x, abs_y);
    /* 子控件由递归绘制处理 */
}

/* 递归绘制控件树 */
void es_widget_draw(es_widget_t *widget)
{
    if (!widget || !widget->visible) return;

    int abs_x, abs_y;
    es_widget_absolute_pos(widget, &abs_x, &abs_y);

    /* 根据类型绘制 */
    switch (widget->type) {
        case ES_WIDGET_LABEL:
            draw_label(widget, abs_x, abs_y);
            break;
        case ES_WIDGET_BUTTON:
            draw_button(widget, abs_x, abs_y);
            break;
        case ES_WIDGET_CONTAINER:
            draw_container(widget, abs_x, abs_y);
            break;
        default:
            draw_background(widget, abs_x, abs_y);
            break;
    }

    /* 递归绘制子控件 */
    for (int i = 0; i < widget->child_count; i++) {
        es_widget_draw(widget->children[i]);
    }
}

void es_widget_draw_all(void)
{
    if (s_root) {
        es_widget_draw(s_root);
    }
}

/* ========== 事件处理 ========== */

/* 查找点击位置所在的控件（从最内层到最外层） */
static es_widget_t *hit_test(es_widget_t *widget, int abs_x, int abs_y, int px, int py)
{
    if (!widget || !widget->visible || !widget->enabled) return NULL;

    int wx, wy;
    es_widget_absolute_pos(widget, &wx, &wy);

    /* 检查是否在控件范围内 */
    if (px < wx || px >= wx + widget->width || py < wy || py >= wy + widget->height) {
        return NULL;
    }

    /* 先检查子控件（从最后一个开始，绘制顺序在上层） */
    for (int i = widget->child_count - 1; i >= 0; i--) {
        es_widget_t *hit = hit_test(widget->children[i], abs_x, abs_y, px, py);
        if (hit) return hit;
    }

    return widget;
}

bool es_widget_handle_event(es_widget_t *widget, es_event_t *event)
{
    if (!widget || !widget->visible || !widget->enabled) return false;
    if (!widget->on_event) return false;

    return widget->on_event(widget, event);
}

bool es_widget_handle_touch(int px, int py, es_event_type_t type)
{
    if (!s_root) return false;

    es_widget_t *target = NULL;

    if (type == ES_EVENT_TOUCH_DOWN) {
        target = hit_test(s_root, 0, 0, px, py);
        s_touch_target = target;
    } else if (type == ES_EVENT_TOUCH_UP) {
        target = s_touch_target;
        s_touch_target = NULL;
    } else if (type == ES_EVENT_TOUCH_MOVE) {
        target = s_touch_target;
    }

    if (!target) return false;

    es_event_t event;
    event.type = type;
    event.touch.x = px;
    event.touch.y = py;
    event.touch.target = target;

    /* 沿着控件树向上传递事件 */
    es_widget_t *w = target;
    while (w) {
        if (w->on_event && w->on_event(w, &event)) {
            return true;
        }
        w = w->parent;
    }

    return false;
}

bool es_widget_handle_key(es_key_t key, es_event_type_t type)
{
    if (!s_root) return false;

    es_event_t event;
    event.type = type;
    event.key = key;

    /* 优先发给焦点控件 */
    if (s_focus && s_focus->on_event) {
        if (s_focus->on_event(s_focus, &event)) {
            return true;
        }
    }

    /* 从根控件向下传递 */
    return es_widget_handle_event(s_root, &event);
}

/* ========== 控件查找 ========== */

es_widget_t *es_widget_find_by_id(es_widget_t *root, int id)
{
    if (!root) return NULL;
    if (root->id == id) return root;

    for (int i = 0; i < root->child_count; i++) {
        es_widget_t *found = es_widget_find_by_id(root->children[i], id);
        if (found) return found;
    }
    return NULL;
}

es_widget_t *es_widget_find_by_tag(es_widget_t *root, const char *tag)
{
    if (!root || !tag) return NULL;
    if (root->tag && strcmp(root->tag, tag) == 0) return root;

    for (int i = 0; i < root->child_count; i++) {
        es_widget_t *found = es_widget_find_by_tag(root->children[i], tag);
        if (found) return found;
    }
    return NULL;
}

/* ========== 便捷创建函数 ========== */

es_widget_t *es_label_create(int x, int y, int w, int h, const char *text)
{
    es_widget_t *label = (es_widget_t *)malloc(sizeof(es_widget_t));
    if (!label) return NULL;
    es_widget_init(label, ES_WIDGET_LABEL);
    es_widget_set_frame(label, x, y, w, h);
    label->text = text;
    return label;
}

es_widget_t *es_button_create(int x, int y, int w, int h, const char *text)
{
    es_widget_t *btn = (es_widget_t *)malloc(sizeof(es_widget_t));
    if (!btn) return NULL;
    es_widget_init(btn, ES_WIDGET_BUTTON);
    es_widget_set_frame(btn, x, y, w, h);
    btn->text = text;
    btn->bg_color = ES_COLOR_PRIMARY;
    btn->fg_color = KD_COLOR_WHITE;
    btn->radius = 4;
    return btn;
}

es_widget_t *es_container_create(int x, int y, int w, int h)
{
    es_widget_t *container = (es_widget_t *)malloc(sizeof(es_widget_t));
    if (!container) return NULL;
    es_widget_init(container, ES_WIDGET_CONTAINER);
    es_widget_set_frame(container, x, y, w, h);
    return container;
}