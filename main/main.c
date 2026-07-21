#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include "return_to_loader.h"

static const char *TAG = "DESKTOP";

/* Metro Tile Data */
typedef struct {
    const char *title;
    uint32_t color;
    bool updated;
} metro_tile_t;

static metro_tile_t s_tiles[] = {
    {"System", 0xFF6F00},
    "Apps", 0x3F51B5
};

static lv_obj_t *s_desktop;
static lv_obj_t *s_tile_lockscreen;
static lv_obj_t *s_tile_system;
static lv_obj_t *s_tile_apps;

static void event_handler(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    printf("Tile clicked: %s\n", (const char *)lv_obj_get_user_data(obj));
    lv_obj_add_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *tile_create(const char *title, uint32_t color, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *tile = lv_obj_create(s_desktop);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_radius(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(tile, (void *)title);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    lv_obj_add_event_cb(tile, event_handler, LV_EVENT_CLICKED, NULL);
    return tile;
}

static void desktop_create(void)
{
    s_desktop = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_desktop, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_desktop, lv_color_black(), 0);
    lv_obj_set_style_pad_all(s_desktop, 0, 0);
    lv_obj_clear_flag(s_desktop, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题栏 */
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "xiao\nmiao desktop");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 4);

    /* 时间磁贴 (2x2) */
    tile_create("12:42", 0xFF6F00, 2, 24, 78, 40);

    /* 系统磁贴 (1x1) */
    tile_create("System", 0x3F51B5, 82, 24, 38, 18);

    /* 应用磁贴 (1x1) */
    tile_create("Apps", 0x009688, 122, 24, 36, 18);

    /* 中排 */
    tile_create("File", 0x607D8B, 2, 66, 56, 18);
    tile_create("Settings", 0x795548, 60, 66, 60, 18);

    /* 大应用区 */
    tile_create("Notes", 0xFF9800, 2, 86, 76, 38);
}

static void status_bar_create(void)
{
    lv_obj_t *bar = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(bar, 0, LV_VER_RES - 12);
    lv_obj_set_size(bar, LV_HOR_RES, 12);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A237E), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 1, 0);

    lv_obj_t *time_lbl = lv_label_create(bar);
    lv_label_set_text(time_lbl, "12:42");
    lv_obj_set_style_text_color(time_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t *bat_lbl = lv_label_create(bar);
    lv_label_set_text(bat_lbl, "BAT");
    lv_obj_set_style_text_color(bat_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(bat_lbl, LV_ALIGN_RIGHT_MID, -2, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Metro Desktop starting...");
    return_to_loader_setup();

    lv_init();

    lv_disp_t *disp = lv_disp_create(LV_HOR_RES, LV_VER_RES);
    lv_disp_set_flush_cb(disp, NULL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);

    desktop_create();
    status_bar_create();

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
