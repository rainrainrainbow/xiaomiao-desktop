/**
 * @file bg_tasks.c
 * @brief 后台任务框架实现 — 注册表 + 进度跟踪 + 暂停/继续/取消
 */

#include "bg_tasks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "BG_TASKS";

/* 8 个任务槽位，static 全局，不占栈、不分配堆 */
static bg_task_t s_tasks[BG_TASK_MAX_SLOTS];

/* 保护所有槽位的读写；用户回调一律在释放本锁后调用 */
static SemaphoreHandle_t s_mutex = NULL;

/* 类型/状态中文短名集中定义，后续可替换为 lang_get() */
static const char *const s_type_names[] = {
    "音乐播放",     /* BG_TASK_MUSIC */
    "OTA下载",      /* BG_TASK_OTA */
    "应用安装",     /* BG_TASK_APP_INSTALL */
    "文件复制",     /* BG_TASK_FILE_COPY */
    "缓存写入",     /* BG_TASK_CACHE_WRITE */
    "灯效动画",     /* BG_TASK_LED_ANIM */
    "蓝牙连接",     /* BG_TASK_BT_CONNECT */
};

static const char *const s_state_names[] = {
    "空闲",         /* BG_TASK_STATE_IDLE */
    "运行中",       /* BG_TASK_STATE_RUNNING */
    "已暂停",       /* BG_TASK_STATE_PAUSED */
    "已完成",       /* BG_TASK_STATE_DONE */
    "出错",         /* BG_TASK_STATE_ERROR */
    "已取消",       /* BG_TASK_STATE_CANCELED */
};

#define STATE_NAME_COUNT ((int)(sizeof(s_state_names) / sizeof(s_state_names[0])))

/* ========== 内部工具 ========== */

/** 安全拷贝文本：保证以 '\0' 结尾，src 为 NULL 时清空 */
static void copy_text(char *dst, const char *src, size_t dst_size)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static bool id_valid(int id)
{
    return (id >= 0 && id < BG_TASK_MAX_SLOTS);
}

typedef enum {
    CMD_PAUSE = 0,
    CMD_RESUME,
    CMD_CANCEL,
} bg_cmd_t;

/**
 * 统一的暂停/继续/取消实现：
 * 1. 加锁读取状态、取出回调与 ctx
 * 2. 释放锁后调用回调（避免持锁调用用户代码）
 * 3. 回调返回 false 时回滚状态
 */
static esp_err_t bg_task_command(int id, bg_cmd_t cmd)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (!id_valid(id)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bg_task_t *t = &s_tasks[id];
    if (!t->active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    bg_task_state_t prev = t->state;
    bool (*cb)(void *) = NULL;
    void *ctx = NULL;

    switch (cmd) {
    case CMD_PAUSE:
        if (prev == BG_TASK_STATE_PAUSED) {           /* 已暂停：幂等 */
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        if (prev != BG_TASK_STATE_RUNNING) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        t->state = BG_TASK_STATE_PAUSED;
        cb = t->on_pause;
        ctx = t->ctx;
        break;

    case CMD_RESUME:
        if (prev == BG_TASK_STATE_RUNNING) {          /* 已运行：幂等 */
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        if (prev != BG_TASK_STATE_PAUSED) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        t->state = BG_TASK_STATE_RUNNING;
        cb = t->on_resume;
        ctx = t->ctx;
        break;

    case CMD_CANCEL:
        if (prev == BG_TASK_STATE_CANCELED) {         /* 已取消：幂等 */
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
        t->state = BG_TASK_STATE_CANCELED;
        cb = t->on_cancel;
        ctx = t->ctx;
        break;

    default:
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    bg_task_state_t applied = t->state;
    xSemaphoreGive(s_mutex);                          /* 先释放锁，再调回调 */

    if (cb != NULL) {
        bool ok = cb(ctx);
        if (!ok) {
            /* 回调失败：仅当状态未被回调自身改写时才回滚 */
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_tasks[id].active && s_tasks[id].state == applied) {
                s_tasks[id].state = prev;
            }
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "task %d callback failed, state reverted", id);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* ========== 公共 API ========== */

esp_err_t bg_tasks_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;                                /* 幂等 */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(s_tasks, 0, sizeof(s_tasks));
    ESP_LOGI(TAG, "initialized, %d slots", BG_TASK_MAX_SLOTS);
    return ESP_OK;
}

int bg_tasks_register(bg_task_type_t type, const char *title, void *ctx,
                      bool (*on_pause)(void *), bool (*on_resume)(void *), bool (*on_cancel)(void *))
{
    if (s_mutex == NULL) return -1;
    if ((int)type < 0 || (int)type >= BG_TASK_MAX) return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int id = -1;

    /* 1) 同类型已存在：视为更新 */
    for (int i = 0; i < BG_TASK_MAX_SLOTS; i++) {
        if (s_tasks[i].active && s_tasks[i].type == type) {
            id = i;
            break;
        }
    }

    /* 2) 否则找一个空闲槽位 */
    if (id < 0) {
        for (int i = 0; i < BG_TASK_MAX_SLOTS; i++) {
            if (!s_tasks[i].active) {
                id = i;
                break;
            }
        }
    }

    if (id >= 0) {
        bg_task_t *t = &s_tasks[id];
        if (!t->active) {
            /* 新任务 */
            memset(t, 0, sizeof(*t));
            t->active = true;
            t->type = type;
            t->state = BG_TASK_STATE_RUNNING;
            t->progress = 0;
        } else {
            /* 同类型任务在终态/空闲态被重新注册：按新任务重启 */
            if (t->state == BG_TASK_STATE_IDLE ||
                t->state == BG_TASK_STATE_DONE ||
                t->state == BG_TASK_STATE_ERROR ||
                t->state == BG_TASK_STATE_CANCELED) {
                t->state = BG_TASK_STATE_RUNNING;
                t->progress = 0;
                t->detail[0] = '\0';
            }
            /* RUNNING/PAUSED：保留进度与状态，仅更新元信息 */
        }
        copy_text(t->title, title, sizeof(t->title));
        t->ctx = ctx;
        t->on_pause = on_pause;
        t->on_resume = on_resume;
        t->on_cancel = on_cancel;
    } else {
        ESP_LOGW(TAG, "no free slot for type %d", (int)type);
    }

    xSemaphoreGive(s_mutex);
    return id;
}

void bg_tasks_set_progress(int id, int progress, const char *detail)
{
    if (s_mutex == NULL || !id_valid(id)) return;
    if (progress < 0) progress = 0;
    if (progress > 100) progress = 100;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bg_task_t *t = &s_tasks[id];
    if (t->active) {
        t->progress = progress;
        copy_text(t->detail, detail, sizeof(t->detail));
    }
    xSemaphoreGive(s_mutex);
}

void bg_tasks_set_state(int id, bg_task_state_t state)
{
    if (s_mutex == NULL || !id_valid(id)) return;
    if ((int)state < 0 || (int)state >= STATE_NAME_COUNT) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_tasks[id].active) {
        s_tasks[id].state = state;
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t bg_tasks_pause(int id)
{
    return bg_task_command(id, CMD_PAUSE);
}

esp_err_t bg_tasks_resume(int id)
{
    return bg_task_command(id, CMD_RESUME);
}

esp_err_t bg_tasks_cancel(int id)
{
    return bg_task_command(id, CMD_CANCEL);
}

void bg_tasks_remove(int id)
{
    if (s_mutex == NULL || !id_valid(id)) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_tasks[id], 0, sizeof(s_tasks[id]));     /* active 清零即释放槽位 */
    xSemaphoreGive(s_mutex);
}

int bg_tasks_snapshot(bg_task_t *out, int max_count)
{
    if (s_mutex == NULL || out == NULL || max_count <= 0) return 0;

    int n = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < BG_TASK_MAX_SLOTS && n < max_count; i++) {
        if (s_tasks[i].active) {
            out[n++] = s_tasks[i];
        }
    }
    xSemaphoreGive(s_mutex);
    return n;
}

int bg_tasks_find(bg_task_type_t type)
{
    if (s_mutex == NULL) return -1;
    if ((int)type < 0 || (int)type >= BG_TASK_MAX) return -1;

    int id = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < BG_TASK_MAX_SLOTS; i++) {
        if (s_tasks[i].active && s_tasks[i].type == type) {
            id = i;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return id;
}

const char *bg_tasks_type_name(bg_task_type_t type)
{
    if ((int)type < 0 || (int)type >= BG_TASK_MAX) return "未知";
    return s_type_names[(int)type];
}

const char *bg_tasks_state_name(bg_task_state_t state)
{
    if ((int)state < 0 || (int)state >= STATE_NAME_COUNT) return "未知";
    return s_state_names[(int)state];
}
