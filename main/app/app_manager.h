/**
 * @file app_manager.h
 * @brief 应用管理器 - 管理内置应用、SD卡应用、MicroPython应用
 */

#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include "ui_framework.h"
#include <stdbool.h>
#include <stdint.h>

/* ========== 应用类型 ========== */
typedef enum {
    APP_TYPE_BUILTIN = 0,     // 内置C应用
    APP_TYPE_MICROPYTHON,     // MicroPython应用
    APP_TYPE_MAX
} app_type_t;

/* ========== 应用定义 ========== */
typedef struct {
    const char *name;         // 应用名称
    const char *icon_text;    // 图标文字（1-2字符）
    uint32_t icon_color;      // 图标颜色
    app_type_t type;          // 应用类型
    
    // 内置应用：启动回调
    void (*launch_cb)(void);
    
    // MicroPython应用：入口文件路径
    const char *py_entry;
} app_def_t;

/* ========== 应用管理器接口 ========== */

/**
 * 初始化应用管理器
 */
void app_manager_init(void);

/**
 * 注册内置应用
 * @param app 应用定义
 */
void app_register_builtin(const app_def_t *app);

/**
 * 获取内置应用列表
 * @param count 输出应用数量
 * @return 应用数组指针
 */
const app_def_t* app_manager_get_builtin(int *count);

/**
 * 获取MicroPython应用列表
 * @param count 输出应用数量
 * @return 应用数组指针
 */
const app_def_t* app_manager_get_micropython(int *count);

/**
 * 扫描SD卡中的MicroPython应用
 * @return 扫描到的应用数量
 */
int app_manager_scan_sdcard(void);

/**
 * 启动应用
 * @param app 应用定义
 */
void app_manager_launch(const app_def_t *app);

/**
 * 获取应用页面回调
 * @param app 应用定义
 * @return 页面回调结构体
 */
page_callbacks_t app_manager_get_callbacks(const app_def_t *app);

/**
 * 注册所有内置应用
 */
void app_builtin_register_all(void);

/**
 * 根据应用名获取内置应用页面回调
 * @param app_name 应用名（"Settings", "Phone" 等）
 * @return 页面回调指针，未找到返回 NULL
 */
const page_callbacks_t* app_builtin_get_callbacks(const char *app_name);

/* ========== 最近任务（Recents） ========== */

#define MAX_RECENTS  6

/**
 * 记录一个最近打开的应用
 * @param app 应用定义
 */
void app_manager_add_recents(const app_def_t *app);

/**
 * 获取最近打开的应用列表（最近优先）
 * @param count 输出数量
 * @return 应用数组指针（最多 MAX_RECENTS 个）
 */
const app_def_t* app_manager_get_recents(int *count);

/**
 * 获取最近任务的第 i 个应用指针
 * @param i 索引（0=最近打开）
 * @return 应用指针，越界返回 NULL
 */
const app_def_t* app_manager_get_recents_at(int i);

/**
 * 获取当前正在运行的应用名称
 * @return 应用名，桌面时返回 NULL
 */
const char* app_manager_get_current_name(void);

/**
 * 清除当前应用名（回到桌面时调用）
 */
void app_manager_clear_current(void);

/**
 * 桌面"应用"图标点击时，直接进入设置中的应用管理二级页面
 */
void app_launch_app_manager(void);

#endif /* APP_MANAGER_H */