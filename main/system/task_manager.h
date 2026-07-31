#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "lvgl.h"
#include <stdbool.h>

#define MAX_TASKS 16

typedef struct {
    char id[24];
    char name[24];
    const char *icon;
    lv_color_t icon_bg;
    bool locked;
    uint32_t memory_kb;
    lv_obj_t *(*create_func)(lv_obj_t *parent);
} task_entry_t;

void task_manager_init(void);
int task_manager_get_count(void);
task_entry_t *task_manager_get_task(int index);
int task_manager_find_task(const char *id);
void task_manager_add_task(const char *id, const char *name, const char *icon,
                           lv_color_t icon_bg, lv_obj_t *(*create_func)(lv_obj_t *parent));
void task_manager_remove_task(int index);
void task_manager_remove_task_by_id(const char *id);
void task_manager_toggle_lock(int index);
bool task_manager_is_locked(int index);
void task_manager_clean_all(void);
int task_manager_get_locked_count(void);
int task_manager_get_unlocked_count(void);

#endif /* TASK_MANAGER_H */