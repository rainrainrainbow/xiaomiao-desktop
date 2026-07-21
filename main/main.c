#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include "return_to_loader.h"

extern void tiles_render(void);
extern void dock_render(void);

static const char *TAG = "DESKTOP";

static void status_bar_render(void)
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
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(time_lbl, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t *bat_lbl = lv_label_create(bar);
    lv_label_set_text(bat_lbl, "BAT");
    lv_obj_set_style_text_color(bat_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(bat_lbl, &lv_font_montserrat_14, 0);
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

    /* 标题 */
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "xiao\nmiao desktop");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 4);

    /* 部件 */
    tiles_render();
    dock_render();
    status_bar_render();

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
