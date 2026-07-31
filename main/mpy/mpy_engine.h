#ifndef MPY_ENGINE_H
#define MPY_ENGINE_H

#include "esp_err.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief MicroPython Runtime Engine
 *
 * 四层架构中的第四层 — MicroPython 运行时层。
 * 基于 espressif/micropython IDF 官方组件。
 *
 * 职责：
 *   1. MicroPython 解释器初始化与生命周期管理 (mp_init / mp_deinit)
 *   2. 注册 xiaomiao 硬件模块到 Python 命名空间
 *   3. 从 SD 卡扫描并加载 .app 应用包
 *   4. 执行 Python 脚本并提供 C↔Python 桥接
 */

/* ===== App Package (.app) =====
 * .app 是 SD 卡中的目录，结构如下：
 *   /sdcard/apps/snake.app/
 *     ├── manifest.json   — 应用元数据
 *     ├── main.py         — 主入口脚本
 *     ├── icon.png        — 图标（可选，最大 32x32）
 *     └── lib/            — 纯 Python 依赖库（可选）
 */

#define MPY_APP_ID_MAX       32
#define MPY_APP_NAME_MAX     48
#define MPY_APP_PATH_MAX     128
#define MPY_MAX_APPS         32
#define MPY_MAX_SCRIPT_SIZE  (48 * 1024)   /* 48KB max per script */
#define MPY_APP_TASK_STACK   (24 * 1024)   /* 24KB stack for MicroPython task */

/* ===== App Package Metadata ===== */
typedef struct {
    char id[MPY_APP_ID_MAX];
    char name[MPY_APP_NAME_MAX];
    char version[16];
    char author[32];
    char icon_emoji[8];          /* Fallback emoji */
    char path[MPY_APP_PATH_MAX]; /* Full path, e.g. /sdcard/apps/snake.app */
    bool has_icon;               /* icon.png exists */
} mpy_app_t;

/* ===== Engine Lifecycle ===== */
esp_err_t mpy_init(void);
esp_err_t mpy_deinit(void);
bool      mpy_is_running(void);

/* ===== Script Execution ===== */
esp_err_t mpy_exec(const char *script);
esp_err_t mpy_exec_file(const char *path);

/* ===== App Package Management ===== */
int       mpy_scan_apps(const char *dir, mpy_app_t *apps, int max);
esp_err_t mpy_parse_manifest(const char *json, mpy_app_t *app);
esp_err_t mpy_load_app_icon(const mpy_app_t *app, uint8_t *buf, size_t max, size_t *out);

/* ===== App Launch ===== */
esp_err_t mpy_launch_app(lv_obj_t *container, const mpy_app_t *app);
void      mpy_stop_app(void);

/* ===== C ↔ Python Bridge Callbacks ===== */
typedef void (*mpy_key_cb_t)(uint8_t key, bool pressed);
typedef void (*mpy_lcd_cb_t)(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *pixels);

void mpy_set_key_callback(mpy_key_cb_t cb);
void mpy_set_lcd_callback(mpy_lcd_cb_t cb);

/* ===== Python-to-C Event Dispatch ===== */
/* Called from the LVGL task to let MicroPython process pending events */
void mpy_process_events(void);

/* ===== Hardware Module Registration ===== */
esp_err_t mpy_register_hardware_modules(void);

#endif /* MPY_ENGINE_H */