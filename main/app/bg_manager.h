/**
 * @file bg_manager.h
 * @brief 后台应用管理器 — 多后台运行支持
 *
 * 功能：
 * - 跟踪已启动的应用的状态（前台/后台/已终止）
 * - 支持应用挂起到后台后恢复运行（保留状态）
 * - 最近任务页面展示真正在运行的后台应用
 *
 * 设计原则：
 * - 不修改页面栈本身的行为（pop时仍会销毁LVGL对象）
 * - 利用各应用的static变量天然持久化特性保持逻辑状态
 * - 每个应用可在init中检查"是否已在后台运行"来跳过状态重置
 */

#ifndef BG_MANAGER_H
#define BG_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

/* ========== 后台应用状态 ========== */
typedef enum {
    BG_SLOT_EMPTY = 0,      // 空槽位
    BG_STATE_FOREGROUND,     // 前台运行中
    BG_STATE_BACKGROUND,     // 后台挂起中（UI已销毁，逻辑状态保留在static变量）
} bg_state_t;

#define MAX_BG_APPS 8

/**
 * 初始化后台管理器
 */
void bg_manager_init(void);

/**
 * 注册应用启动（在app_manager_launch中调用）
 * @param name 应用名（必须持久存在，通常来自app_def_t.name）
 * 标记该应用为前台运行。如果已在表中则更新为前台。
 */
void bg_manager_on_launch(const char *name);

/**
 * 挂起当前前台应用到后台（B键返回桌面时调用）
 * 将前台状态的应用标记为后台。
 */
void bg_manager_suspend_current(void);

/**
 * 终止应用（彻底关闭，从表中移除）
 */
void bg_manager_kill(const char *name);

/**
 * 获取后台运行应用数量（前台+后台都算）
 */
int bg_manager_get_count(void);

/**
 * 获取第i个运行中的应用名（0=最新）
 * @return 应用名指针，越界返回NULL
 */
const char* bg_manager_get_name(int idx);

/**
 * 获取第i个运行中的应用状态
 */
bg_state_t bg_manager_get_state(int idx);

/**
 * 检查指定应用是否正在运行（前台或后台）
 */
bool bg_manager_is_running(const char *name);

#endif /* BG_MANAGER_H */
