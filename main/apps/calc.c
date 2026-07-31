#include "xiaomiao_desktop.h"
#include "esp_log.h"

static const char *TAG = "calc";

lv_obj_t *calc_create(lv_obj_t *parent)
{
    ESP_LOGI(TAG, "Calculator launched");

    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(container, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);

    lv_obj_t *title = lv_label_create(container);
    lv_label_set_text(title, "🔢 计算器");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_BROWN), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(container);
    lv_label_set_text(hint, "B退出");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_10, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 20);

    return container;
}