/**
 * @file bg_tasks.h
 * @brief 后台任务框架 — 任务注册 / 进度跟踪 / 暂停 / 继续 / 取消 / 重试
 *
 * 与 bg_manager.c（应用前后台状态表，用于"最近任务"页）不同：
 * 本模块是真正的后台任务注册表，供音乐播放、OTA 下载、文件复制等
 * 耗时业务登记进度，UI 通过快照接口渲染后台任务列表。
 *
 * 设计要点：
 * - 固定 8 个槽位，bg_task_t 数组为 static 全局，不占栈、不动态分配
 * - 标题/详情使用固定长度 char 数组，节省内部 RAM
 * - 所有读写由 FreeRTOS mutex 保护；用户回调在锁外调用
 */

#ifndef BG_TASKS_H
#define BG_TASKS_H

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 最多同时跟踪的后台任务数（即任务 id 范围 0~7） */
#define BG_TASK_MAX_SLOTS 8

/* ========== 任务类型 ========== */
typedef enum {
    BG_TASK_MUSIC = 0,      /* 音乐播放 */
    BG_TASK_OTA,            /* OTA下载 */
    BG_TASK_APP_INSTALL,    /* 应用安装 */
    BG_TASK_FILE_COPY,      /* 文件复制/移动 */
    BG_TASK_CACHE_WRITE,    /* 缓存写入 */
    BG_TASK_LED_ANIM,       /* 灯效动画 */
    BG_TASK_BT_CONNECT,     /* 蓝牙连接 */
    BG_TASK_MAX
} bg_task_type_t;

/* ========== 任务状态 ========== */
typedef enum {
    BG_TASK_STATE_IDLE = 0,
    BG_TASK_STATE_RUNNING,
    BG_TASK_STATE_PAUSED,
    BG_TASK_STATE_DONE,
    BG_TASK_STATE_ERROR,
    BG_TASK_STATE_CANCELED
} bg_task_state_t;

/* ========== 任务槽位 ========== */
typedef struct {
    bg_task_type_t type;
    bg_task_state_t state;
    int progress;           /* 0-100 */
    char title[24];         /* 任务名，短 */
    char detail[24];        /* 进度/速度/剩余时间文本 */
    bool active;            /* 槽位是否占用 */
    void *ctx;              /* 业务侧上下文（可选） */
    bool (*on_pause)(void *ctx);    /* 暂停回调（可选） */
    bool (*on_resume)(void *ctx);   /* 继续回调（可选） */
    bool (*on_cancel)(void *ctx);   /* 取消回调（可选） */
} bg_task_t;

/* ========== API ========== */

/** 初始化注册表（幂等）； mutex 创建失败返回 ESP_ERR_NO_MEM */
esp_err_t bg_tasks_init(void);

/**
 * 注册/更新一个任务，返回任务 id（0-7），失败返回 -1。
 * - 同类型任务已存在：更新标题与回调，若处于非运行态则重置为 RUNNING、进度清零
 * - 否则分配空闲槽位；无空闲槽位返回 -1
 */
int bg_tasks_register(bg_task_type_t type, const char *title, void *ctx,
                      bool (*on_pause)(void *), bool (*on_resume)(void *), bool (*on_cancel)(void *));

/** 设置任务进度（自动钳制到 0-100）和状态文本 */
void bg_tasks_set_progress(int id, int progress, const char *detail);

/** 设置任务状态 */
void bg_tasks_set_state(int id, bg_task_state_t state);

/**
 * 暂停/继续/取消，调用对应回调（回调在锁外执行）。
 * 回调返回 false 时回滚状态并返回 ESP_FAIL。
 */
esp_err_t bg_tasks_pause(int id);
esp_err_t bg_tasks_resume(int id);
esp_err_t bg_tasks_cancel(int id);

/** 移除任务（释放槽位，不触发取消回调） */
void bg_tasks_remove(int id);

/** 获取任务快照（只读），count 返回实际拷贝数量；用于 UI 列表渲染 */
int bg_tasks_snapshot(bg_task_t *out, int max_count);

/** 获取某类型的任务 id，无则 -1 */
int bg_tasks_find(bg_task_type_t type);

/** 类型名字符串（中文短名），非法值返回"未知" */
const char *bg_tasks_type_name(bg_task_type_t type);

/** 状态名字符串（中文短名），非法值返回"未知" */
const char *bg_tasks_state_name(bg_task_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* BG_TASKS_H */
