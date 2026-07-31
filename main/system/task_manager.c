#include "task_manager.h"
#include "string.h"
#include "esp_log.h"

static const char *TAG = "task_manager";

static task_entry_t s_tasks[MAX_TASKS];
static int s_task_count = 0;

void task_manager_init(void)
{
    s_task_count = 0;
    memset(s_tasks, 0, sizeof(s_tasks));
    ESP_LOGI(TAG, "Task manager initialized");
}

int task_manager_get_count(void)
{
    return s_task_count;
}

task_entry_t *task_manager_get_task(int index)
{
    if (index < 0 || index >= s_task_count) return NULL;
    return &s_tasks[index];
}

int task_manager_find_task(const char *id)
{
    for (int i = 0; i < s_task_count; i++) {
        if (strcmp(s_tasks[i].id, id) == 0) return i;
    }
    return -1;
}

void task_manager_add_task(const char *id, const char *name, const char *icon,
                           lv_color_t icon_bg, lv_obj_t *(*create_func)(lv_obj_t *parent))
{
    if (s_task_count >= MAX_TASKS) {
        ESP_LOGW(TAG, "Task list full, cannot add %s", name);
        return;
    }
    /* Check if already exists */
    if (task_manager_find_task(id) >= 0) return;

    task_entry_t *t = &s_tasks[s_task_count];
    strncpy(t->id, id, sizeof(t->id) - 1);
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->icon = icon;
    t->icon_bg = icon_bg;
    t->locked = false;
    t->memory_kb = (rand() % 200) + 50;
    t->create_func = create_func;
    s_task_count++;
    ESP_LOGI(TAG, "Task added: %s (%s)", name, id);
}

void task_manager_remove_task(int index)
{
    if (index < 0 || index >= s_task_count) return;
    if (s_tasks[index].locked) return;

    for (int i = index; i < s_task_count - 1; i++) {
        s_tasks[i] = s_tasks[i + 1];
    }
    s_task_count--;
    memset(&s_tasks[s_task_count], 0, sizeof(task_entry_t));
}

void task_manager_remove_task_by_id(const char *id)
{
    int idx = task_manager_find_task(id);
    if (idx >= 0) task_manager_remove_task(idx);
}

void task_manager_toggle_lock(int index)
{
    if (index < 0 || index >= s_task_count) return;
    s_tasks[index].locked = !s_tasks[index].locked;
}

bool task_manager_is_locked(int index)
{
    if (index < 0 || index >= s_task_count) return false;
    return s_tasks[index].locked;
}

void task_manager_clean_all(void)
{
    int removed = 0;
    for (int i = s_task_count - 1; i >= 0; i--) {
        if (!s_tasks[i].locked) {
            task_manager_remove_task(i);
            removed++;
        }
    }
    ESP_LOGI(TAG, "Cleaned %d tasks, %d locked remain", removed, s_task_count);
}

int task_manager_get_locked_count(void)
{
    int count = 0;
    for (int i = 0; i < s_task_count; i++) {
        if (s_tasks[i].locked) count++;
    }
    return count;
}

int task_manager_get_unlocked_count(void)
{
    return s_task_count - task_manager_get_locked_count();
}