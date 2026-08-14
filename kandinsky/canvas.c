/**
 * @file kandinsky/canvas.c
 * @brief Kandinsky - Graphics Engine: Canvas Implementation
 *
 * 画布操作实现，在 PSRAM 帧缓冲区上进行像素/直线/矩形/圆绘制。
 * 采用 NumWorks Epsilon 风格：颜色为 RGB565，坐标系原点在左上角。
 * 所有绘制操作针对 PSRAM 帧缓冲区，完成后调用 ion_display_flush() 刷新。
 */

#include "kandinsky/canvas.h"
#include "ion/display.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ========== 内部状态 ========== */
static ion_color_t *s_fb = NULL;           /* 帧缓冲区指针（PSRAM） */
static int s_width = ION_DISPLAY_WIDTH;
static int s_height = ION_DISPLAY_HEIGHT;

/* 裁剪区域（默认全屏） */
static int s_clip_x = 0;
static int s_clip_y = 0;
static int s_clip_w = ION_DISPLAY_WIDTH;
static int s_clip_h = ION_DISPLAY_HEIGHT;

/* ========== 颜色混合辅助 ========== */

/* 将 RGB565 拆分为 R/G/B 分量 */
#define RGB565_R(c) (((c) >> 11) & 0x1F)
#define RGB565_G(c) (((c) >> 5)  & 0x3F)
#define RGB565_B(c) ((c)         & 0x1F)

/* 合成 RGB565 */
#define RGB565(r, g, b) ((ion_color_t)(((r) << 11) | ((g) << 5) | (b)))

/* 将 5/6 位分量扩展到 8 位 */
#define EXPAND5(v) ((v) << 3 | (v) >> 2)   /* 5-bit -> 8-bit */
#define EXPAND6(v) ((v) << 2 | (v) >> 4)   /* 6-bit -> 8-bit */

/* ========== 初始化 ========== */

void kd_canvas_init(ion_color_t *framebuffer)
{
    s_fb = framebuffer;
    /* 全屏清空为黑色 */
    if (s_fb) {
        for (int i = 0; i < s_width * s_height; i++) {
            s_fb[i] = KD_COLOR_BLACK;
        }
    }
}

void kd_canvas_clear(ion_color_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < s_width * s_height; i++) {
        s_fb[i] = color;
    }
}

/* ========== 裁剪区域 ========== */

void kd_canvas_set_clip(int x, int y, int w, int h)
{
    /* 限制在屏幕范围内 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_width)  w = s_width - x;
    if (y + h > s_height) h = s_height - y;

    s_clip_x = x;
    s_clip_y = y;
    s_clip_w = w;
    s_clip_h = h;
}

void kd_canvas_reset_clip(void)
{
    s_clip_x = 0;
    s_clip_y = 0;
    s_clip_w = s_width;
    s_clip_h = s_height;
}

/* 检查像素是否在裁剪区域内 */
static inline bool is_clipped(int x, int y)
{
    return (x >= s_clip_x && x < s_clip_x + s_clip_w &&
            y >= s_clip_y && y < s_clip_y + s_clip_h);
}

/* ========== 像素操作 ========== */

void kd_canvas_set_pixel(int x, int y, ion_color_t color)
{
    if (!s_fb) return;
    if (x < 0 || x >= s_width || y < 0 || y >= s_height) return;
    if (!is_clipped(x, y)) return;
    s_fb[y * s_width + x] = color;
}

ion_color_t kd_canvas_get_pixel(int x, int y)
{
    if (!s_fb) return 0;
    if (x < 0 || x >= s_width || y < 0 || y >= s_height) return 0;
    return s_fb[y * s_width + x];
}

/* ========== 直线绘制（Bresenham 算法） ========== */

void kd_canvas_draw_line(int x0, int y0, int x1, int y1, ion_color_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        kd_canvas_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ========== 矩形绘制 ========== */

void kd_canvas_draw_rect(int x, int y, int w, int h, ion_color_t color)
{
    /* 上边 */
    kd_canvas_draw_line(x, y, x + w - 1, y, color);
    /* 下边 */
    kd_canvas_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    /* 左边 */
    kd_canvas_draw_line(x, y, x, y + h - 1, color);
    /* 右边 */
    kd_canvas_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void kd_canvas_fill_rect(int x, int y, int w, int h, ion_color_t color)
{
    if (!s_fb) return;

    /* 裁剪 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= s_width || y >= s_height) return;
    if (x + w > s_width)  w = s_width - x;
    if (y + h > s_height) h = s_height - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        int offset = row * s_width + x;
        for (int col = 0; col < w; col++) {
            s_fb[offset + col] = color;
        }
    }
}

/* ========== 圆角矩形 ========== */

void kd_canvas_fill_round_rect(int x, int y, int w, int h, int r, ion_color_t color)
{
    if (r <= 0) {
        kd_canvas_fill_rect(x, y, w, h, color);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* 主体矩形（去掉四个角） */
    kd_canvas_fill_rect(x + r, y, w - 2 * r, h, color);           /* 中间 */
    kd_canvas_fill_rect(x, y + r, r, h - 2 * r, color);           /* 左边 */
    kd_canvas_fill_rect(x + w - r, y + r, r, h - 2 * r, color);   /* 右边 */

    /* 绘制四个圆角（Bresenham 圆算法） */
    for (int cy = -r; cy <= r; cy++) {
        for (int cx = -r; cx <= r; cx++) {
            if (cx * cx + cy * cy <= r * r) {
                /* 左上角 */
                kd_canvas_set_pixel(x + r + cx, y + r + cy, color);
                /* 右上角 */
                kd_canvas_set_pixel(x + w - r - 1 + cx, y + r + cy, color);
                /* 左下角 */
                kd_canvas_set_pixel(x + r + cx, y + h - r - 1 + cy, color);
                /* 右下角 */
                kd_canvas_set_pixel(x + w - r - 1 + cx, y + h - r - 1 + cy, color);
            }
        }
    }
}

/* ========== 圆形绘制 ========== */

void kd_canvas_draw_circle(int cx, int cy, int r, ion_color_t color)
{
    int x = 0, y = r;
    int d = 3 - 2 * r;

    while (x <= y) {
        kd_canvas_set_pixel(cx + x, cy + y, color);
        kd_canvas_set_pixel(cx - x, cy + y, color);
        kd_canvas_set_pixel(cx + x, cy - y, color);
        kd_canvas_set_pixel(cx - x, cy - y, color);
        kd_canvas_set_pixel(cx + y, cy + x, color);
        kd_canvas_set_pixel(cx - y, cy + x, color);
        kd_canvas_set_pixel(cx + y, cy - x, color);
        kd_canvas_set_pixel(cx - y, cy - x, color);

        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void kd_canvas_fill_circle(int cx, int cy, int r, ion_color_t color)
{
    for (int y = -r; y <= r; y++) {
        int dx = (int)sqrt(r * r - y * y);
        kd_canvas_draw_line(cx - dx, cy + y, cx + dx, cy + y, color);
    }
}

/* ========== 位图绘制 ========== */

void kd_canvas_draw_bitmap(int x, int y, int w, int h, const ion_color_t *data)
{
    if (!s_fb || !data) return;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            ion_color_t pixel = data[row * w + col];
            /* 跳过透明像素（0x0000 作为透明色） */
            if (pixel != 0x0000) {
                kd_canvas_set_pixel(x + col, y + row, pixel);
            }
        }
    }
}

/* ========== 图像缩放 ========== */

void kd_canvas_draw_bitmap_scaled(int x, int y, int dw, int dh,
                                   const ion_color_t *src, int sw, int sh)
{
    if (!s_fb || !src) return;

    for (int dy = 0; dy < dh; dy++) {
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            int sy = dy * sh / dh;
            ion_color_t pixel = src[sy * sw + sx];
            if (pixel != 0x0000) {
                kd_canvas_set_pixel(x + dx, y + dy, pixel);
            }
        }
    }
}

/* ========== 颜色工具 ========== */

ion_color_t kd_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return RGB565(r >> 3, g >> 2, b >> 3);
}

void kd_color_split(ion_color_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = EXPAND5(RGB565_R(color));
    if (g) *g = EXPAND6(RGB565_G(color));
    if (b) *b = EXPAND5(RGB565_B(color));
}

ion_color_t kd_color_blend(ion_color_t fg, ion_color_t bg, uint8_t alpha)
{
    uint8_t fr, fg_r, fb;
    uint8_t br, bg_r, bb;
    kd_color_split(fg, &fr, &fg_r, &fb);
    kd_color_split(bg, &br, &bg_r, &bb);

    uint8_t rr = (fr * alpha + br * (255 - alpha)) / 255;
    uint8_t rg = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    uint8_t rb = (fb * alpha + bb * (255 - alpha)) / 255;

    return kd_color_rgb(rr, rg, rb);
}