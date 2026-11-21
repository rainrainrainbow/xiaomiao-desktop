/**
 * @file app_sys_api.h
 * @brief 应用系统 API 门面层（App System Facade）
 *
 * 设计目标：
 *   让新增/维护一个内置 App 时，只需要 include 本头文件，即可获得所有
 *   常用系统能力（配置读写、系统状态、导航、通用 UI 组件、文件扫描），
 *   而不必逐个 include 各底层驱动与框架头文件。
 *
 * 分层位置：
 *   [App: setting/store/music/shell/files/led]
 *                    ↓ 只依赖这一层
 *   [app_sys_api]  ← 本文件（门面 / facade）
 *                    ↓ 转发到底层
 *   [sys_nvs / driver / ion / ui_framework / app_manager]
 *
 * 新增加一个 App 的标准步骤：
 *   1. 新建 app_xxx.c，#include "app_sys_api.h"
 *   2. 实现 init/activate/destroy/on_key，导出 const page_callbacks_t g_xxx_callbacks
 *   3. 在 app_builtin.h 声明、app_builtin.c 的 defs[] 与 get_callbacks() 各加一行
 *   4. 在 main/CMakeLists.txt 的 SRCS 里加一行
 *   无需了解底层驱动细节。
 */
#ifndef APP_SYS_API_H
#define APP_SYS_API_H

#include "ui_framework.h"      /* page_callbacks_t / page_type_t / key_type_t / lvgl */
#include <stdbool.h>
#include <stdint.h>

/* ============================================================
 * 一、系统状态查询
 * ============================================================ */

/** 电池电量百分比 (0-100) */
uint8_t app_sys_battery_percent(void);

/** 电池电压 (mV) */
uint16_t app_sys_battery_mv(void);

/** SD 卡是否已挂载 */
bool app_sys_sdcard_ready(void);

/** 内部 DRAM 空闲字节数 */
uint32_t app_sys_heap_internal_free(void);

/** PSRAM 空闲字节数 */
uint32_t app_sys_psram_free(void);

/** 固件版本号字符串（如 "v71"） */
const char *app_sys_version(void);

/* ============================================================
 * 二、用户配置读写（统一 NVS 门面）
 * ============================================================ */

/** 配置项键（语义化枚举，屏蔽底层 NVS key 字符串） */
typedef enum {
    APP_CFG_BRIGHTNESS = 0,
    APP_CFG_VOLUME,
    APP_CFG_SOUND_ON,
    APP_CFG_THEME,
    APP_CFG_WIFI_ON,
    APP_CFG_LAYOUT,
    APP_CFG_FONT_SIZE,
    APP_CFG_SLEEP_TIMEOUT,
    APP_CFG_LANGUAGE,
    APP_CFG_FONT_SOURCE,
    APP_CFG_AUDIO_OUT,
    APP_CFG_MUSIC_EQ,
    APP_CFG_MUSIC_LRC,
    APP_CFG_MUSIC_MODE,
    APP_CFG_MAX
} app_cfg_key_t;

/** 读取整数型配置，未设置时返回 default_val */
int  app_sys_cfg_get_int(app_cfg_key_t key, int default_val);

/** 写入整数型配置（同时更新内存中的 ui_state，各 setter 负责持久化到 NVS） */
void app_sys_cfg_set_int(app_cfg_key_t key, int value);

/** 保存 UI 全局状态到 NVS（批量保存场景，如设置页退出） */
void app_sys_cfg_save_all(void);

/* ============================================================
 * 三、应用导航
 * ============================================================ */

/** 推入一个应用页面（统一入口，内部处理 title 设置） */
void app_sys_nav_push(page_type_t type, const page_callbacks_t *cb, const char *title);

/** 返回上一级（栈深>1 时弹栈） */
void app_sys_nav_back(void);

/** 回到桌面 */
void app_sys_nav_home(void);

/** 设置状态栏标题 */
void app_sys_set_title(const char *title);

/* ============================================================
 * 四、通用 UI 组件
 * ============================================================ */

/**
 * 创建标准应用页面骨架：全屏容器 + 状态栏 + 标题栏，
 * 返回内容区父对象（已按字体自适应定位），App 只需往里填内容。
 * @param title 标题栏文字
 * @return 内容区 lv_obj_t*（失败返回 NULL）
 */
lv_obj_t *app_sys_page_create(const char *title);

/**
 * 创建统一风格的列表容器（带主题背景、滚动条），供设置/商店/文件等复用。
 */
lv_obj_t *app_sys_list_create(lv_obj_t *parent);

/**
 * 向列表追加一行文本项，返回 label 对象便于后续刷新。
 */
lv_obj_t *app_sys_list_add(lv_obj_t *list, const char *text);

/**
 * 统一的"选中行高亮"：先清掉所有行背景，再高亮指定行。
 * rows 为 app_sys_list_add 返回的 label 数组，count 为行数，sel 为选中索引。
 */
void app_sys_list_highlight(lv_obj_t **rows, int count, int sel);

/* ============================================================
 * 五、文件/目录辅助
 * ============================================================ */

/** 路径是否为目录 */
bool app_sys_fs_is_dir(const char *path);

/** 路径是否为普通文件 */
bool app_sys_fs_is_file(const char *path);

/** 文件名是否具有指定后缀（大小写不敏感），如 ".py" ".mp3" */
bool app_sys_fs_has_ext(const char *name, const char *ext);

#endif /* APP_SYS_API_H */
