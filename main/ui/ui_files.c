#include "ui_widgets.h"
#include "xiaomiao_desktop.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "ui_files";

/* Virtual file system for demo */
typedef struct {
    const char *name;
    const char *type; /* "folder", "file", "app" */
    const char *size;
} fs_entry_t;

static const fs_entry_t s_root[] = {
    { "apps", "folder", "-" },
    { "roms", "folder", "-" },
    { "music", "folder", "-" },
    { "config.json", "file", "2KB" },
};

static const fs_entry_t s_apps[] = {
    { "snake.app", "app", "48KB" },
    { "calc.app", "app", "32KB" },
    { "music.app", "app", "64KB" },
};

static const fs_entry_t s_roms[] = {
    { "game1.bin", "file", "1.2MB" },
    { "game2.bin", "file", "800KB" },
};

static const fs_entry_t s_music[] = {
    { "bgm1.mp3", "file", "3.5MB" },
    { "bgm2.mp3", "file", "4.2MB" },
};

static const fs_entry_t *s_current_dir = s_root;
static int s_current_count = 4;
static char s_current_path[64] = "/sdcard";
static int s_path_depth = 0;

static void file_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const fs_entry_t *f = &s_current_dir[idx];

    if (strcmp(f->type, "folder") == 0) {
        if (strcmp(f->name, "apps") == 0) {
            s_current_dir = s_apps;
            s_current_count = 3;
            s_path_depth = 1;
        } else if (strcmp(f->name, "roms") == 0) {
            s_current_dir = s_roms;
            s_current_count = 2;
            s_path_depth = 1;
        } else if (strcmp(f->name, "music") == 0) {
            s_current_dir = s_music;
            s_current_count = 2;
            s_path_depth = 1;
        }
        snprintf(s_current_path, sizeof(s_current_path), "/sdcard/%s", f->name);
        lv_obj_t *scr = lv_screen_active();
        lv_obj_clean(scr);
        ui_files_create(scr);
    } else if (strcmp(f->type, "app") == 0) {
        ui_show_toast("安装中...");
    } else {
        ui_show_toast("打开...");
    }
}

void ui_files_create(lv_obj_t *scr)
{
    char title[32];
    snprintf(title, sizeof(title), "文件: %s", s_current_path);
    static const char *hints[] = {"↑↓选择", "A打开", "B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, title, hints, 3);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, LCD_H_RES, CONTENT_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_current_count; i++) {
        const fs_entry_t *f = &s_current_dir[i];

        lv_obj_t *item = lv_button_create(list);
        lv_obj_set_size(item, LCD_H_RES - 8, 24);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 2, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_pad_all(item, 4, 0);

        const char *icon = "📄";
        if (strcmp(f->type, "folder") == 0) icon = "📁";
        else if (strcmp(f->type, "app") == 0) icon = "📦";

        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s  %s", icon, f->name, f->size);
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        lv_obj_center(lbl);

        lv_group_add_obj(g_group, item);
        lv_obj_add_event_cb(item, file_item_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    }
    ESP_LOGI(TAG, "Files page: %s (%d items)", s_current_path, s_current_count);
}

/* Called from nav_back to reset file state */
void ui_files_reset(void)
{
    s_current_dir = s_root;
    s_current_count = 4;
    s_path_depth = 0;
    strcpy(s_current_path, "/sdcard");
}