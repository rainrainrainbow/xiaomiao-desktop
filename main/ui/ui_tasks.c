#include "ui_widgets.h"
#include "xiaomiao_desktop.h"
#include "task_manager.h"
#include "esp_log.h"

static const char *TAG = "ui_tasks";

static void task_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    task_manager_toggle_lock(idx);
    /* Rebuild the list */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_tasks_create(scr);
}

static void task_clean_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (task_manager_is_locked(idx)) {
        ui_show_toast("已锁定，无法清理");
        return;
    }
    task_entry_t *t = task_manager_get_task(idx);
    if (t) {
        task_manager_remove_task(idx);
        lv_obj_t *scr = lv_screen_active();
        lv_obj_clean(scr);
        ui_tasks_create(scr);
    }
}

static void clean_all_cb(lv_event_t *e)
{
    int locked = task_manager_get_locked_count();
    int total = task_manager_get_count();
    task_manager_clean_all();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    ui_tasks_create(scr);
}

static void task_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_LEFT) {
        clean_all_cb(e);
    } else if (key == LV_KEY_RIGHT) {
        /* Get the focused item index */
        lv_obj_t *focused = lv_group_get_focused(g_group);
        if (focused) {
            /* Find the index */
            lv_obj_t *parent = lv_obj_get_parent(focused);
            for (int i = 0; i < lv_obj_get_child_cnt(parent); i++) {
                if (lv_obj_get_child(parent, i) == focused) {
                    int idx = i;
                    task_entry_t *t = task_manager_get_task(idx);
                    if (t && !t->locked) {
                        task_manager_remove_task(idx);
                        lv_obj_t *scr = lv_screen_active();
                        lv_obj_clean(scr);
                        ui_tasks_create(scr);
                    } else if (t) {
                        ui_show_toast("已锁定，无法清理");
                    }
                    break;
                }
            }
        }
    }
}

void ui_tasks_create(lv_obj_t *scr)
{
    static const char *hints[] = {"↑↓选择", "A锁定", "→清理", "←全清", "B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, "后台任务", hints, 5);

    int count = task_manager_get_count();
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d个应用运行中", count);

    /* Show count */
    lv_obj_t *count_lbl = lv_label_create(content);
    lv_label_set_text(count_lbl, count_str);
    lv_obj_set_style_text_color(count_lbl, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_text_font(count_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(count_lbl, LV_ALIGN_TOP_MID, 0, 2);

    /* Task list */
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, LCD_H_RES, CONTENT_H - 14);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 14);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < count; i++) {
        task_entry_t *t = task_manager_get_task(i);
        if (!t) continue;

        lv_obj_t *item = lv_obj_create(list);
        lv_obj_set_size(item, LCD_H_RES - 8, 28);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 2, 0);
        lv_obj_set_style_border_color(item, t->locked ? lv_color_hex(UI_GREEN) : lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_pad_all(item, 4, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Icon */
        lv_obj_t *icon = lv_label_create(item);
        lv_label_set_text(icon, t->icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);

        /* Name and memory */
        lv_obj_t *info = lv_label_create(item);
        char info_buf[48];
        snprintf(info_buf, sizeof(info_buf), "%s %s", t->name, t->locked ? "🔒" : "");
        lv_label_set_text(info, info_buf);
        lv_obj_set_style_text_font(info, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(info, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_color(info, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        lv_obj_set_flex_grow(info, 1);

        /* Memory info */
        lv_obj_t *mem = lv_label_create(item);
        char mem_buf[16];
        snprintf(mem_buf, sizeof(mem_buf), "%luKB", (unsigned long)t->memory_kb);
        lv_label_set_text(mem, mem_buf);
        lv_obj_set_style_text_font(mem, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(mem, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_color(mem, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);

        lv_group_add_obj(g_group, item);

        /* A = toggle lock */
        lv_obj_add_event_cb(item, task_click_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
        /* Also handle key events */
        lv_obj_add_event_cb(item, task_key_cb, LV_EVENT_KEY, NULL);
    }
    ESP_LOGI(TAG, "Tasks page: %d running", count);
}