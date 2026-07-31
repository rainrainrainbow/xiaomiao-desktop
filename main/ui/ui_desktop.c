#include "ui_widgets.h"
#include "xiaomiao_desktop.h"
#include "app_launcher.h"
#include "esp_log.h"

static const char *TAG = "ui_desktop";

static void desktop_app_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    app_launcher_launch(idx);
}

void ui_desktop_create(lv_obj_t *scr)
{
    static const char *hints[] = {"↑↓←→选择", "A打开", "B后台"};
    lv_obj_t *content = ui_create_standard_layout(scr, "小喵桌面", hints, 3);

    /* Create 3×2 app grid */
    lv_obj_t *grid = lv_obj_create(content);
    lv_obj_set_size(grid, LCD_H_RES, CONTENT_H);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(grid, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    /* Grid: 3 columns, 2 rows */
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    int app_count = app_launcher_get_count();
    for (int i = 0; i < app_count && i < 6; i++) {
        const app_entry_t *app = app_launcher_get_app(i);

        lv_obj_t *btn = lv_button_create(grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                                   LV_GRID_ALIGN_STRETCH, i / 3, 1);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Icon */
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, app->icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);

        /* Name */
        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, app->name);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);

        /* Add to group for keyboard navigation */
        lv_group_add_obj(g_group, btn);

        /* Event: launch app on click */
        lv_obj_add_event_cb(btn, desktop_app_click_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    }
    ESP_LOGI(TAG, "Desktop created with %d apps", app_count < 6 ? app_count : 6);
}