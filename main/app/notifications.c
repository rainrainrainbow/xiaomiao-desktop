/**
 * @file notifications.c
 * @brief 通知中心实现 — 定长环形缓冲 + FreeRTOS 互斥锁
 */

#include "notifications.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define NOTIFY_MAX 12

static const char *TAG = "NOTIFY";

/* 环形缓冲：id 即物理槽位下标（0~11），push 返回值可直接用于 mark_read */
static notification_t s_notifs[NOTIFY_MAX];
static uint32_t s_seq[NOTIFY_MAX];      /* 发布序号，用于同一秒内的先后比较 */
static int s_read_idx = 0;              /* 最旧条目下标 */
static int s_write_idx = 0;             /* 下一条写入下标 */
static int s_count = 0;                 /* 当前有效条数 */
static uint32_t s_next_seq = 0;         /* 自增发布序号 */
static SemaphoreHandle_t s_mutex = NULL;

/* 类别图标：单字节 ASCII，避免中文字库/编码问题 */
static const char s_category_icons[] = {
    'D',    /* DOWNLOAD 下载 */
    'U',    /* UPDATE   升级 */
    'I',    /* INSTALL  安装 */
    'C',    /* CONNECT  连接 */
    'S',    /* STORAGE  存储 */
    'N',    /* GENERAL  通用 */
};

/* 类别中文短名：集中定义，替换多语言(lang)时只改此处 */
static const char *const s_category_names[] = {
    "下载", /* DOWNLOAD */
    "升级", /* UPDATE   */
    "安装", /* INSTALL  */
    "连接", /* CONNECT  */
    "存储", /* STORAGE  */
    "通知", /* GENERAL  */
};

/* 校正类别越界，统一回落到 GENERAL */
static notify_category_t sanitize_cat(notify_category_t cat)
{
    if (cat < NOTIFY_CATEGORY_DOWNLOAD || cat > NOTIFY_CATEGORY_GENERAL) {
        return NOTIFY_CATEGORY_GENERAL;
    }
    return cat;
}

esp_err_t notifications_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;  /* 已初始化，避免重复创建锁 */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_FAIL;
    }
    memset(s_notifs, 0, sizeof(s_notifs));
    memset(s_seq, 0, sizeof(s_seq));
    s_read_idx = 0;
    s_write_idx = 0;
    s_count = 0;
    s_next_seq = 0;
    ESP_LOGI(TAG, "Notification center initialized, capacity=%d", NOTIFY_MAX);
    return ESP_OK;
}

int notifications_push(notify_category_t cat, const char *title, int jump_target)
{
    if (s_mutex == NULL) return -1;             /* 未初始化 */
    cat = sanitize_cat(cat);

    int id;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    id = s_write_idx;
    notification_t *n = &s_notifs[s_write_idx];

    /* 推进写指针；缓冲已满时读指针同步前移，实现覆盖最旧 */
    s_write_idx = (s_write_idx + 1) % NOTIFY_MAX;
    if (s_count < NOTIFY_MAX) {
        s_count++;
    } else {
        s_read_idx = (s_read_idx + 1) % NOTIFY_MAX;
    }

    n->id = id;             /* 物理槽位 id，供 UI snapshot 后单条标记已读 */
    n->category = cat;
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    n->timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    n->read = false;
    n->jump_target = jump_target;
    s_seq[id] = s_next_seq++;

    xSemaphoreGive(s_mutex);
    return id;
}

int notifications_unread_count(void)
{
    if (s_mutex == NULL) return 0;
    int cnt = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (!s_notifs[(s_read_idx + i) % NOTIFY_MAX].read) {
            cnt++;
        }
    }
    xSemaphoreGive(s_mutex);
    return cnt;
}

int notifications_count(void)
{
    if (s_mutex == NULL) return 0;
    int cnt;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cnt = s_count;
    xSemaphoreGive(s_mutex);
    return cnt;
}

int notifications_snapshot(notification_t *out, int max_count)
{
    if (s_mutex == NULL || out == NULL || max_count <= 0) return 0;

    int idx[NOTIFY_MAX];                /* 12 个 int = 48 字节，栈开销极小 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int limit = s_count < max_count ? s_count : max_count;
    for (int i = 0; i < limit; i++) {
        idx[i] = (s_read_idx + i) % NOTIFY_MAX;
    }

    /* 插入排序：时间新者在前；同一秒内按发布序号大者（更晚发布）在前 */
    for (int i = 1; i < limit; i++) {
        int key = idx[i];
        int j = i - 1;
        while (j >= 0 &&
               (s_notifs[idx[j]].timestamp < s_notifs[key].timestamp ||
                (s_notifs[idx[j]].timestamp == s_notifs[key].timestamp &&
                 s_seq[idx[j]] < s_seq[key]))) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    /* 只在锁内读取并拷贝，不持锁调用任何回调（本框架无回调） */
    for (int i = 0; i < limit; i++) {
        out[i] = s_notifs[idx[i]];
    }

    xSemaphoreGive(s_mutex);
    return limit;
}

void notifications_mark_read(int id)
{
    if (s_mutex == NULL || id < 0 || id >= NOTIFY_MAX) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_notifs[id].read = true;
    xSemaphoreGive(s_mutex);
}

void notifications_mark_all_read(void)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        s_notifs[(s_read_idx + i) % NOTIFY_MAX].read = true;
    }
    xSemaphoreGive(s_mutex);
}

void notifications_clear(void)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_notifs, 0, sizeof(s_notifs));
    memset(s_seq, 0, sizeof(s_seq));
    s_read_idx = 0;
    s_write_idx = 0;
    s_count = 0;
    s_next_seq = 0;
    xSemaphoreGive(s_mutex);
}

char notifications_category_icon(notify_category_t cat)
{
    return s_category_icons[sanitize_cat(cat)];
}

const char *notifications_category_name(notify_category_t cat)
{
    return s_category_names[sanitize_cat(cat)];
}
