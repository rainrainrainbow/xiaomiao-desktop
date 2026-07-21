#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "DOCK";

static void dock_item_event_handler(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    ESP_LOGI(TAG, "DOCK ITEM pressed: %s", (const char *)lv_obj_get_user_data(obj));
}

void dock_render(void)
{
    lv_obj_t *dock = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(dock, 0, LV_VER_RES - 22);
    lv_obj_set_size(dock, LV_HOR_RES, 20);
    lv_obj_set_style_bg_opa(dock, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(dock, lv_color_black(), 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_pad_all(dock, 2, 0);
    lv_obj_set_style_radius(dock, 0, 0);

    const char *items[] = {"Phone", "Mail", "Photos", NULL};
    int gap = 4;
    int item_w = 24;
    int total_w = (int)(sizeof(items) / sizeof(items[0]) - 1) * gap + (int)(sizeof(items) / sizeof(items[0]) - 1) * item_w;
    int start_x = (LV_HOR_RES - total_w) / 2;

    for (int i = 0; items[i] != NULL; i++) {
        lv_obj_t *btn = lv_btn_create(dock);
        lv_obj_set_size(btn, item_w, 16);
        lv_obj_set_pos(btn, start_x + i * (item_w + gap), 2);
        lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_set_style_border_width(btn, 0, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, items[i]);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_center(label);

        lv_obj_set_user_data(btn, (void *)items[i]);
        lv_obj_add_event_cb(btn, dock_item_event_handler, LV_EVENT_CLICKED, NULL);
    }
}
