/**
 * @file app_micropython.c
 * @brief MicroPython 应用层集成 - 委托给 poincare/runtime 新架构实现
 * 
 * 本文件是旧应用层（main/app）与新架构 Poincaré 运行时（poincare/runtime）的
 * 适配层。MicroPython 核心运行时、脚本执行、NLR jump fail 处理、native code
 * commit 等在 poincare/runtime.c 中统一实现，此处仅保留应用页面回调和扫描逻辑。
 *
 * 这样消除了 app_micropython.c 与 poincare/runtime.c 之间的重复代码和潜在
 * 重复符号（nlr_jump_fail、esp_native_code_commit 等），符合渐进式迁移方向。
 */

#include "app_manager.h"
#include "app_micropython.h"
#include "ui_framework.h"
#include "poincare/runtime.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP_PY";

/* ========== MicroPython 运行时 API（委托给 poincare/runtime） ========== */

/**
 * @brief 初始化 MicroPython 运行时（幂等，可重复调用）
 * @return true 成功，false 失败
 */
bool app_micropython_init(void)
{
    /* 委托给 Poincaré 运行时（默认 64KB PSRAM GC 堆） */
    return poincare_runtime_init(0);
}

/**
 * @brief 检查 MicroPython 是否已初始化
 */
bool app_micropython_is_ready(void)
{
    return poincare_runtime_is_ready();
}

/**
 * @brief 执行一段 Python 源码字符串
 */
int app_micropython_exec(const char *source, const char *source_name)
{
    return poincare_runtime_exec(source, source_name ? source_name : "<string>");
}

/**
 * @brief 执行一个 Python 文件
 */
int app_micropython_exec_file(const char *filename)
{
    return poincare_runtime_exec_file(filename);
}

/* ========== MicroPython应用页面回调 ========== */

static void python_app_init(void *data);
static void python_app_activate(void);
static void python_app_destroy(void);
static bool python_app_on_key(int key);

static const page_callbacks_t s_python_callbacks = {
    .init = python_app_init,
    .activate = python_app_activate,
    .destroy = python_app_destroy,
    .on_key = python_app_on_key,
};

/* ========== MicroPython应用初始化 ========== */
static void python_app_init(void *data)
{
    const app_def_t *app = (const app_def_t *)data;
    if (!app) {
        ESP_LOGE(TAG, "NULL app data");
        return;
    }

    ESP_LOGI(TAG, "Python app init: %s", app->name);
    /* 初始化 MicroPython 运行时（委托给 Poincaré） */
    app_micropython_init();
}

static void python_app_activate(void)
{
    ESP_LOGI(TAG, "Python app activate");

    /* 在屏幕上显示 MicroPython 测试信息 */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_titlebar_create(scr, 14, "Python 测试");

    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);

    /* 执行测试脚本 */
    int ret = app_micropython_exec("print('Hello from XiaoMiao MicroPython!')\n", "<boot>");

    /* 显示测试结果 */
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_xiaomiao_cn_14, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -8);

    if (ret == 0) {
        lv_label_set_text(lbl, "MicroPython 运行正常!\nHello from XiaoMiao!");
        ESP_LOGI(TAG, "MicroPython test PASSED");
    } else {
        lv_label_set_text(lbl, "MicroPython 测试失败\n请查看串口日志");
        ESP_LOGE(TAG, "MicroPython test FAILED");
    }

    ui_dock_create(scr, 1, 0);
}

static void python_app_destroy(void)
{
    ESP_LOGI(TAG, "Python app destroy");
}

static bool python_app_on_key(int key)
{
    if (key == KEY_B) {
        if (ui_stack_depth() > 1) {
            ui_stack_pop();
        }
        return true;
    }
    return false;
}

/* ========== 获取Python应用页面回调 ========== */
const page_callbacks_t* app_micropython_get_callbacks(void)
{
    return &s_python_callbacks;
}

/* ========== 扫描SD卡Python应用 ========== */
int app_micropython_scan(const char *base_path, app_def_t *apps, int max_count)
{
    ESP_LOGW(TAG, "MicroPython scan not implemented yet");
    return 0;
}