/**
 * @file notifications.h
 * @brief 通知中心 — 后台通知框架（下载完成/升级可用/安装完成/连接断开/空间不足等）
 *
 * 特性：
 * - 定长环形缓冲（12 条），满时覆盖最旧，RAM 占用固定可控
 * - 业务线程发布(push)、UI 线程读取(snapshot)，内部用 FreeRTOS mutex 保护
 * - 支持已读标记、全部已读、清空、跳转目标标识
 *
 * 用法：
 *   1) 上电调用 notifications_init()
 *   2) 各业务模块调用 notifications_push(...) 发布
 *   3) 后台 UI 页面调用 notifications_snapshot(...) 取倒序快照展示
 */

#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通知类别 */
typedef enum {
    NOTIFY_CATEGORY_DOWNLOAD = 0,   /* 下载完成 */
    NOTIFY_CATEGORY_UPDATE,         /* 升级可用 */
    NOTIFY_CATEGORY_INSTALL,        /* 安装完成 */
    NOTIFY_CATEGORY_CONNECT,        /* 连接断开 */
    NOTIFY_CATEGORY_STORAGE,        /* 空间不足 */
    NOTIFY_CATEGORY_GENERAL         /* 通用 */
} notify_category_t;

/* 单条通知 */
typedef struct {
    int id;                 /* 物理槽位 id（0~11），用于 notifications_mark_read() */
    notify_category_t category;
    char title[40];         /* 通知内容，短文本 */
    uint32_t timestamp;     /* esp_timer_get_time()/1e6 秒 */
    bool read;              /* 已读标志 */
    int jump_target;        /* 跳转目标标识，业务自定义，-1=无 */
} notification_t;

/* ========== API ========== */

/** 初始化通知中心（创建互斥锁、清零缓冲），重复调用安全 */
esp_err_t notifications_init(void);

/**
 * 发布通知
 * @param cat         类别
 * @param title       通知短文本（安全截断，防溢出）
 * @param jump_target 跳转目标标识，-1=无
 * @return 通知 id（0~11 环形槽位），失败返回 -1
 */
int notifications_push(notify_category_t cat, const char *title, int jump_target);

/** 获取未读数量 */
int notifications_unread_count(void);

/** 获取总通知数量（0~12） */
int notifications_count(void);

/**
 * 取通知快照（按时间倒序，最新在前）
 * @param out       输出缓冲，由调用方提供
 * @param max_count out 可容纳的最大条数
 * @return 实际写入条数
 */
int notifications_snapshot(notification_t *out, int max_count);

/** 标记单条已读（id 为 notifications_push 返回的 id） */
void notifications_mark_read(int id);

/** 全部标记已读 */
void notifications_mark_all_read(void);

/** 清空全部通知 */
void notifications_clear(void);

/** 类别图标字符（单字节 ASCII，用于列表前缀） */
char notifications_category_icon(notify_category_t cat);

/** 类别名称（中文短名，集中定义便于替换多语言） */
const char *notifications_category_name(notify_category_t cat);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFICATIONS_H */
