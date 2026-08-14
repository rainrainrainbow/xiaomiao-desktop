/**
 * @file examples/arch-demo/main.c
 * @brief 新架构验证示例 - Metro UI Demo
 *
 * 使用 Ion + Kandinsky + Escher 新架构绘制一个简单的 Metro UI 界面。
 * 验证：显示驱动、画布绘制、字体渲染、Escher 控件树、主题系统。
 * 编译方式：替换 main/main.c 中的 app_main 或作为独立测试。
 *
 * 运行效果：
 * - 蓝色渐变背景（Ion 显示驱动）
 * - 欢迎文字（Kandinsky 字体渲染）
 * - 三个按钮控件（Escher 控件树）
 * - Metro 主题（Escher 主题系统）
 * - 电量显示（Ion 电池驱动）
 */

#include "ion/display.h"
#include "ion/button.h"
#include "ion/battery.h"
#include "kandinsky/canvas.h"
#include "kandinsky/font.h"
#include "escher/widget.h"
#include "escher/layout.h"
#include "escher/theme.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ARCH_DEMO";

/* ========== 帧缓冲区（PSRAM 40KB） ========== */
#define FB_SIZE (ION_DISPLAY_WIDTH * ION_DISPLAY_HEIGHT * sizeof(ion_color_t))
static ion_color_t *s_framebuffer = NULL;

/* ========== 初始化显示 ========== */
static void init_display(void)
{
    /* 初始化 Ion 显示驱动 */
    if (!ion_display_init()) {
        ESP_LOGE(TAG, "Failed to init display");
        return;
    }

    /* 在 PSRAM 分配帧缓冲区 */
    s_framebuffer = (ion_color_t *)heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_framebuffer) {
        ESP_LOGW(TAG, "PSRAM alloc failed, using DRAM");
        s_framebuffer = (ion_color_t *)malloc(FB_SIZE);
    }

    /* 初始化 Kandinsky 画布 */
    kd_canvas_init(s_framebuffer);

    /* 设置默认字体为 5x7 */
    kd_font_set_default(&kd_font_5x7);

    ESP_LOGI(TAG, "Display initialized (%dx%d, fb=%p)", 
             ION_DISPLAY_WIDTH, ION_DISPLAY_HEIGHT, s_framebuffer);
}

/* ========== 绘制 Metro UI 示例 ========== */
static void draw_metro_ui(void)
{
    const es_theme_t *theme = es_theme_get_current();

    /* 清屏为背景色 */
    kd_canvas_clear(theme->colors.background);

    /* ===== 状态栏（顶部） ===== */
    kd_canvas_fill_rect(0, 0, ION_DISPLAY_WIDTH, 10, theme->colors.surface);

    /* 状态栏文字 - 左侧时间 */
    kd_font_draw_string(2, 1, NULL, "12:00", theme->colors.text_primary, ES_COLOR_TRANSPARENT);

    /* 状态栏文字 - 右侧电池 */
    if (ion_battery_is_ready()) {
        uint8_t bat = ion_battery_get_percentage();
        char bat_str[8];
        snprintf(bat_str, sizeof(bat_str), "%d%%", bat);
        int bw = kd_font_string_width(NULL, bat_str);
        kd_font_draw_string(ION_DISPLAY_WIDTH - bw - 2, 1, NULL, 
                            bat_str, theme->colors.success, ES_COLOR_TRANSPARENT);
    }

    /* ===== 标题区域 ===== */
    kd_canvas_fill_rect(0, 10, ION_DISPLAY_WIDTH, 22, theme->colors.primary);
    kd_font_draw_string(4, 14, NULL, "XiaoMiao OS", KD_COLOR_WHITE, ES_COLOR_TRANSPARENT);

    /* ===== 应用网格（3列x2行） ===== */
    int cols = 3;
    int rows = 2;
    int cell_w = 40;
    int cell_h = 34;
    int gap = 4;
    int grid_x = (ION_DISPLAY_WIDTH - (cols * cell_w + (cols - 1) * gap)) / 2;
    int grid_y = 38;

    /* 应用图标数据（emoji 替代） */
    const char *app_names[] = {"设置", "文件", "商店", "游戏", "工具", "帮助"};
    ion_color_t app_colors[] = {
        KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),  /* 蓝 */
        KD_COLOR_FROM_RGB(0x10, 0xB9, 0x81),  /* 绿 */
        KD_COLOR_FROM_RGB(0xE8, 0x48, 0x10),  /* 橙 */
        KD_COLOR_FROM_RGB(0xBB, 0x86, 0xFC),  /* 紫 */
        KD_COLOR_FROM_RGB(0xFF, 0xA0, 0x00),  /* 黄 */
        KD_COLOR_FROM_RGB(0x00, 0x7A, 0xCC),  /* 蓝 */
    };

    for (int i = 0; i < cols * rows && i < 6; i++) {
        int row = i / cols;
        int col = i % cols;
        int x = grid_x + col * (cell_w + gap);
        int y = grid_y + row * (cell_h + gap);

        /* 应用图标方块 */
        kd_canvas_fill_round_rect(x, y, cell_w, cell_h, 4, theme->colors.surface);
        kd_canvas_fill_round_rect(x + 4, y + 4, cell_w - 8, cell_h - 24, 4, app_colors[i]);

        /* 应用名称 */
        kd_font_draw_string(x + 2, y + cell_h - 9, NULL, 
                            app_names[i], theme->colors.text_primary, ES_COLOR_TRANSPARENT);
    }

    /* ===== 底部导航栏（Dock） ===== */
    int dock_y = ION_DISPLAY_HEIGHT - 8;
    kd_canvas_fill_rect(0, dock_y, ION_DISPLAY_WIDTH, 8, theme->colors.surface);

    /* 分页指示点 */
    int dots = 2;
    for (int i = 0; i < dots; i++) {
        int dot_x = ION_DISPLAY_WIDTH / 2 - (dots * 6) / 2 + i * 6;
        kd_canvas_fill_circle(dot_x, dock_y + 4, 2, 
                              (i == 0) ? theme->colors.primary : theme->colors.text_disabled);
    }

    /* 刷新显示 */
    ion_display_flush();
}

/* ========== Escher 控件树示例 ========== */
static void demo_escher_widgets(void)
{
    /* 创建一个容器作为根控件 */
    es_widget_t *root = es_container_create(0, 0, ION_DISPLAY_WIDTH, ION_DISPLAY_HEIGHT);
    root->bg_color = ES_COLOR_BACKGROUND;
    es_widget_set_root(root);

    /* 创建标题标签 */
    es_widget_t *title = es_label_create(0, 12, ION_DISPLAY_WIDTH, 14, "Escher Demo");
    title->bg_color = ES_COLOR_PRIMARY;
    title->fg_color = KD_COLOR_WHITE;
    title->font = &kd_font_5x7;

    /* 创建三个按钮 */
    es_widget_t *btn1 = es_button_create(10, 40, 100, 20, "Button 1");
    es_widget_t *btn2 = es_button_create(10, 66, 100, 20, "Button 2");
    es_widget_t *btn3 = es_button_create(10, 92, 100, 20, "Button 3");
    btn3->bg_color = ES_COLOR_ACCENT;

    /* 创建信息标签 */
    es_widget_t *info = es_label_create(10, 120, 140, 10, "Press A/B to interact");
    info->align = ES_ALIGN_LEFT;

    /* 构建控件树 */
    es_widget_add_child(root, title);
    es_widget_add_child(root, btn1);
    es_widget_add_child(root, btn2);
    es_widget_add_child(root, btn3);
    es_widget_add_child(root, info);

    /* 绘制控件树 */
    es_widget_draw_all();

    /* 刷新显示 */
    ion_display_flush();
}

/* ========== 主函数 ========== */
void arch_demo_main(void)
{
    ESP_LOGI(TAG, "=== Architecture Demo Started ===");

    /* 初始化硬件层 */
    ion_display_init();
    ion_button_init();
    ion_battery_init();

    /* 分配帧缓冲区 */
    s_framebuffer = (ion_color_t *)heap_caps_malloc(FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_framebuffer) {
        s_framebuffer = (ion_color_t *)malloc(FB_SIZE);
    }

    /* 初始化图形引擎 */
    kd_canvas_init(s_framebuffer);
    kd_font_set_default(&kd_font_5x7);

    /* 初始化主题（Metro 风格） */
    es_theme_init(ES_THEME_METRO);

    ESP_LOGI(TAG, "All subsystems initialized");

    /* 阶段 1：绘制 Metro UI */
    ESP_LOGI(TAG, "Drawing Metro UI...");
    draw_metro_ui();
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 阶段 2：Escher 控件树演示 */
    ESP_LOGI(TAG, "Drawing Escher widget tree...");
    demo_escher_widgets();
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 阶段 3：切换主题演示 */
    ESP_LOGI(TAG, "Switching to Dark theme...");
    es_theme_apply(ES_THEME_DARK);
    draw_metro_ui();
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Demo completed. Entering idle loop.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}