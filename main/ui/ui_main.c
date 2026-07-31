#include "ui_widgets.h"
#include "xiaomiao_desktop.h"
#include "app_launcher.h"
#include "task_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

static const char *TAG = "ui_main";

/* ===== Global State ===== */
lv_display_t *g_display = NULL;
lv_group_t *g_group = NULL;
lv_indev_t *g_indev = NULL;
esp_lcd_panel_io_handle_t g_lcd_io = NULL;

page_id_t g_current_page = PAGE_BOOT;
page_id_t g_prev_page = PAGE_BOOT;

/* Screen objects for each page */
static lv_obj_t *s_screens[PAGE_COUNT];

/* Status bar widgets (set by ui_create_standard_layout) */
lv_obj_t *g_status_time = NULL;
lv_obj_t *g_status_wifi = NULL;
lv_obj_t *g_status_battery = NULL;

/* Focus indices for each page */
static int s_focus_idx[PAGE_COUNT];

/* App running state */
static lv_obj_t *s_app_container = NULL;
static int s_current_app_idx = -1;

/* ===== Standard Layout Builder ===== */
lv_obj_t *ui_create_standard_layout(lv_obj_t *parent, const char *title,
                                    const char *hint_items[], int hint_count)
{
    /* Main container: flex column, full size */
    lv_obj_t *main = lv_obj_create(parent);
    lv_obj_set_size(main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(main, 0, 0);
    lv_obj_set_style_bg_color(main, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(main, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_border_width(main, 0, 0);
    lv_obj_set_flex_flow(main, LV_FLEX_FLOW_COLUMN);

    /* Title bar: 12px high */
    lv_obj_t *title_bar = lv_obj_create(main);
    lv_obj_set_size(title_bar, LCD_H_RES, TITLE_BAR_H);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_flex_grow(title_bar, 0);

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Status bar: 10px high */
    lv_obj_t *status_bar = lv_obj_create(main);
    lv_obj_set_size(status_bar, LCD_H_RES, STATUS_BAR_H);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(status_bar, lv_color_hex(0x4A3218), 0);
    lv_obj_set_style_border_width(status_bar, 1, 0);
    lv_obj_set_flex_grow(status_bar, 0);

    /* Time on left */
    g_status_time = lv_label_create(status_bar);
    lv_label_set_text(g_status_time, "00:00");
    lv_obj_set_style_text_color(g_status_time, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_text_font(g_status_time, &lv_font_montserrat_10, 0);
    lv_obj_align(g_status_time, LV_ALIGN_LEFT_MID, 4, 0);

    /* Battery on right */
    g_status_battery = lv_label_create(status_bar);
    lv_label_set_text(g_status_battery, "🔋85%");
    lv_obj_set_style_text_color(g_status_battery, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_text_font(g_status_battery, &lv_font_montserrat_10, 0);
    lv_obj_align(g_status_battery, LV_ALIGN_RIGHT_MID, -4, 0);

    /* WiFi icon next to battery */
    g_status_wifi = lv_label_create(status_bar);
    lv_label_set_text(g_status_wifi, "📶");
    lv_obj_set_style_text_color(g_status_wifi, lv_color_hex(UI_CREAM), 0);
    lv_obj_align(g_status_wifi, LV_ALIGN_RIGHT_MID, -32, 0);

    /* Content area: flex-grow, scrollable */
    lv_obj_t *content = lv_obj_create(main);
    lv_obj_set_size(content, LCD_H_RES, CONTENT_H);
    lv_obj_set_pos(content, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(content, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    /* Hint bar: 10px high */
    lv_obj_t *hint_bar = lv_obj_create(main);
    lv_obj_set_size(hint_bar, LCD_H_RES, HINT_BAR_H);
    lv_obj_set_style_bg_color(hint_bar, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(hint_bar, 0, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_flex_grow(hint_bar, 0);

    /* Hint items */
    if (hint_items && hint_count > 0) {
        int total_w = 0;
        int *widths = malloc(hint_count * sizeof(int));
        
        for (int i = 0; i < hint_count; i++) {
            /* Create hint label */
            lv_obj_t *hint = lv_label_create(hint_bar);
            lv_label_set_text(hint, hint_items[i]);
            lv_obj_set_style_text_color(hint, lv_color_hex(UI_CREAM), 0);
            lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
            lv_obj_update_layout(hint);
            widths[i] = lv_obj_get_width(hint) + 4;
            total_w += widths[i];
        }
        
        /* Center them */
        int start_x = (LCD_H_RES - total_w) / 2;
        if (start_x < 0) start_x = 0;
        int x = start_x;
        for (int i = 0; i < hint_count; i++) {
            lv_obj_align(hint_bar->child_cnt > 0 ? 
                lv_obj_get_child(hint_bar, lv_obj_get_child_cnt(hint_bar) - (hint_count - i)) : NULL,
                LV_ALIGN_LEFT_MID, x, 0);
            x += widths[i];
        }
        free(widths);
    }

    return content;
}

/* ===== Toast ===== */
static lv_obj_t *s_toast = NULL;

void ui_show_toast(const char *msg)
{
    if (!s_toast) {
        s_toast = lv_label_create(lv_screen_active());
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(UI_BROWN), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_toast, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_pad_all(s_toast, 4, 0);
        lv_obj_set_style_radius(s_toast, 4, 0);
        lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_10, 0);
    }
    lv_label_set_text(s_toast, msg);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Auto-hide after 1.5s */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 1200);
    lv_anim_set_time(&a, 300);
    lv_anim_start(&a);
}

/* ===== Navigation ===== */
void nav_to(page_id_t page)
{
    if (page < 0 || page >= PAGE_COUNT) return;
    g_prev_page = g_current_page;
    g_current_page = page;

    if (s_screens[page]) {
        lv_screen_load(s_screens[page]);
    }

    /* Update focus for the new page - don't clear group, LVGL handles focus naturally */
    ui_update_time();
    ui_update_battery();
    ui_update_wifi_status();
}

void nav_back(void)
{
    nav_to(g_prev_page);
}

/* ===== Status Updates ===== */
void ui_update_time(void)
{
    if (!g_status_time) return;
    time_t now;
    struct tm *tm_info;
    time(&now);
    tm_info = localtime(&now);
    char buf[6];
    sprintf(buf, "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
    lv_label_set_text(g_status_time, buf);
}

void ui_update_battery(void)
{
    if (!g_status_battery) return;
    /* Simulated battery level */
    static int level = 85;
    char buf[16];
    sprintf(buf, "🔋%d%%", level);
    lv_label_set_text(g_status_battery, buf);
}

void ui_update_wifi_status(void)
{
    if (!g_status_wifi) return;
    lv_label_set_text(g_status_wifi, "📶");
}

void ui_update_task_count(void)
{
    /* Called from ui_tasks to update the status bar */
}

/* ===== Keyboard Event Handler ===== */
static void on_key_event(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    lv_obj_t *obj = lv_event_get_target(e);

    if (key == LV_KEY_ESC) {
        /* B button */
        if (g_current_page == PAGE_DESKTOP) {
            nav_to(PAGE_TASKS);
        } else if (g_current_page == PAGE_TASKS) {
            nav_to(PAGE_DESKTOP);
        } else if (g_current_page == PAGE_APP_RUN) {
            nav_to(PAGE_DESKTOP);
        } else {
            nav_to(PAGE_DESKTOP);
        }
    }
}

/* ===== UI Initialization ===== */
void ui_main_init(void)
{
    ESP_LOGI(TAG, "UI main init starting");

    /* Create all screen objects */
    s_screens[PAGE_BOOT] = lv_obj_create(NULL);
    s_screens[PAGE_DESKTOP] = lv_obj_create(NULL);
    s_screens[PAGE_TASKS] = lv_obj_create(NULL);
    s_screens[PAGE_SETTINGS] = lv_obj_create(NULL);
    s_screens[PAGE_WIFI] = lv_obj_create(NULL);
    s_screens[PAGE_DISPLAY] = lv_obj_create(NULL);
    s_screens[PAGE_ABOUT] = lv_obj_create(NULL);
    s_screens[PAGE_FILES] = lv_obj_create(NULL);
    s_screens[PAGE_APP_RUN] = lv_obj_create(NULL);

    /* Initialize focus indices */
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_focus_idx[i] = 0;
    }

    /* Create each page's content */
    ui_boot_create(s_screens[PAGE_BOOT]);
    ui_desktop_create(s_screens[PAGE_DESKTOP]);
    ui_tasks_create(s_screens[PAGE_TASKS]);
    ui_settings_create(s_screens[PAGE_SETTINGS]);
    ui_wifi_create(s_screens[PAGE_WIFI]);
    ui_display_create(s_screens[PAGE_DISPLAY]);
    ui_about_create(s_screens[PAGE_ABOUT]);
    ui_files_create(s_screens[PAGE_FILES]);
    ui_app_run_create(s_screens[PAGE_APP_RUN]);

    /* Add key event handler to the group */
    lv_obj_add_event_cb(s_screens[PAGE_DESKTOP], on_key_event, LV_EVENT_KEY, NULL);

    ESP_LOGI(TAG, "UI main init complete");
}

/* ===== App Run Page ===== */
void ui_app_run_create(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_app_container = lv_obj_create(scr);
    lv_obj_set_size(s_app_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_app_container, 0, 0);
    lv_obj_set_style_bg_color(s_app_container, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_app_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_app_container, 0, 0);
    lv_obj_set_style_pad_all(s_app_container, 0, 0);
}

/* ===== Boot Page ===== */
void ui_boot_create(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Logo */
    lv_obj_t *logo = lv_label_create(scr);
    lv_label_set_text(logo, "🐱");
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_14, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "XiaoMiao OS");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 6);

    /* Progress bar */
    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 140, 14);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_GREEN), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 30);

    /* Animate the progress bar */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_bar_set_value);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2000);
    lv_anim_set_ready_cb(&a, NULL);
    lv_anim_start(&a);
}