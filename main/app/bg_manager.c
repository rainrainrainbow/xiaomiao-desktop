/**
 * @file bg_manager.c
 * @brief 后台应用管理器实现 — 多后台运行支持
 */

#include "bg_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "BG_MGR";

typedef struct {
    const char *name;       // 应用名（指针，指向app_def_t.name，生命周期持久）
    bg_state_t state;       // 运行状态
} bg_slot_t;

static bg_slot_t s_slots[MAX_BG_APPS];
static int s_slot_count = 0;

void bg_manager_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slot_count = 0;
    ESP_LOGI(TAG, "Background manager initialized");
}

void bg_manager_on_launch(const char *name)
{
    if (!name) return;

    // 查找已有条目
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].name && strcmp(s_slots[i].name, name) == 0) {
            // 已存在：更新为前台，移到最前
            s_slots[i].state = BG_STATE_FOREGROUND;
            // 移到队首
            bg_slot_t tmp = s_slots[i];
            for (int j = i; j > 0; j--) {
                s_slots[j] = s_slots[j - 1];
            }
            s_slots[0] = tmp;
            ESP_LOGI(TAG, "App '%s' resumed to foreground", name);
            return;
        }
    }

    // 新增条目
    if (s_slot_count < MAX_BG_APPS) {
        // 移到队首
        for (int j = s_slot_count; j > 0; j--) {
            s_slots[j] = s_slots[j - 1];
        }
        s_slots[0].name = name;
        s_slots[0].state = BG_STATE_FOREGROUND;
        s_slot_count++;
    } else {
        // 满了：淘汰最后一个
        for (int j = MAX_BG_APPS - 1; j > 0; j--) {
            s_slots[j] = s_slots[j - 1];
        }
        s_slots[0].name = name;
        s_slots[0].state = BG_STATE_FOREGROUND;
    }
    ESP_LOGI(TAG, "App '%s' launched (foreground)", name);
}

void bg_manager_suspend_current(void)
{
    // 将前台应用标记为后台
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].state == BG_STATE_FOREGROUND) {
            s_slots[i].state = BG_STATE_BACKGROUND;
            ESP_LOGI(TAG, "App '%s' suspended to background", s_slots[i].name);
            break;
        }
    }
}

void bg_manager_kill(const char *name)
{
    if (!name) return;
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].name && strcmp(s_slots[i].name, name) == 0) {
            ESP_LOGI(TAG, "Killing app '%s'", name);
            for (int j = i; j < s_slot_count - 1; j++) {
                s_slots[j] = s_slots[j + 1];
            }
            s_slot_count--;
            s_slots[s_slot_count].name = NULL;
            s_slots[s_slot_count].state = BG_SLOT_EMPTY;
            return;
        }
    }
}

int bg_manager_get_count(void)
{
    return s_slot_count;
}

const char* bg_manager_get_name(int idx)
{
    if (idx < 0 || idx >= s_slot_count) return NULL;
    return s_slots[idx].name;
}

bg_state_t bg_manager_get_state(int idx)
{
    if (idx < 0 || idx >= s_slot_count) return BG_SLOT_EMPTY;
    return s_slots[idx].state;
}

bool bg_manager_is_running(const char *name)
{
    if (!name) return false;
    for (int i = 0; i < s_slot_count; i++) {
        if (s_slots[i].name && strcmp(s_slots[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}