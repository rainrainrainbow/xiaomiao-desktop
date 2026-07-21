#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "TILES";

static lv_obj_t *tile_create(const char *title, uint32_t color,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *tile = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_radius(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(tile);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    return tile;
}

void tiles_render(void)
{
    /* 时间 */
    tile_create("12:42", 0xFF6F00, 2, 24, 78, 40);

    /* 系统 */
    tile_create("System", 0x3F51B5, 82, 24, 38, 18);
    tile_create("Apps", 0x009688, 122, 24, 36, 18);

    /* 中排 */
    tile_create("File", 0x607D8B, 2, 66, 56, 18);
    tile_create("Settings", 0x795548, 60, 66, 60, 18);

    /* 大应用区 */
    tile_create("Notes", 0xFF9800, 2, 86, 76, 38);
}
