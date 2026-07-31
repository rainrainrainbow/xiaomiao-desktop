#include "ui_widgets.h"
#include "xiaomiao_desktop.h"
#include "esp_log.h"

static const char *TAG = "ui_settings";

static void settings_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    static page_id_t t[] = {PAGE_WIFI, PAGE_DISPLAY, PAGE_ABOUT};
    if (idx >= 0 && idx < 3) nav_to(t[idx]);
}

static void wifi_item_cb(lv_event_t *e)
{
    ui_show_toast("WiFi 连接功能待实现");
}

static void slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(sl);
    lv_obj_t *parent = lv_obj_get_parent(sl);
    /* Find the value label */
    lv_obj_t *lbl = lv_obj_get_child(parent, 2);
    if (lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(lbl, buf);
    }
}

/* ===== Settings Main Page ===== */
void ui_settings_create(lv_obj_t *scr)
{
    static const char *hints[] = {"↑↓选择", "A进入", "B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, "系统设置", hints, 3);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, LCD_H_RES, CONTENT_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    /* Settings items */
    static const char *items[] = {"📶 Wi-Fi", "☀️ 显示设置", "ℹ️ 关于本机"};
    static page_id_t targets[] = {PAGE_WIFI, PAGE_DISPLAY, PAGE_ABOUT};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *item = lv_button_create(list);
        lv_obj_set_size(item, LCD_H_RES - 8, 28);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 2, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_border_color(item, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_pad_all(item, 4, 0);
        lv_obj_set_style_text_color(item, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_color(item, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_10, 0);

        lv_label_set_text(lv_label_create(item), items[i]);
        lv_obj_center(lv_obj_get_child(item, 0));

        lv_group_add_obj(g_group, item);
        lv_obj_add_event_cb(item, settings_item_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    }
}

/* ===== WiFi Page ===== */
void ui_wifi_create(lv_obj_t *scr)
{
    static const char *hints[] = {"↑↓选择", "A连接", "B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, "Wi-Fi", hints, 3);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_set_size(list, LCD_H_RES, CONTENT_H);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    static const char *networks[] = {"Home_WiFi_5G", "XiaoMiao_Guest", "Neighbor_5G", "Coffee_Shop"};
    static bool connected[] = {true, false, false, false};
    static bool locked[] = {true, false, true, false};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *item = lv_button_create(list);
        lv_obj_set_size(item, LCD_H_RES - 8, 24);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(UI_BROWN), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 2, 0);
        lv_obj_set_style_border_color(item, connected[i] ? lv_color_hex(UI_GREEN) : lv_color_hex(UI_CREAM), 0);
        lv_obj_set_style_pad_all(item, 4, 0);

        char buf[48];
        snprintf(buf, sizeof(buf), "📶 %s%s%s", networks[i],
                 connected[i] ? " ✓" : "", locked[i] ? " 🔒" : "");
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_CREAM), LV_STATE_FOCUSED);
        lv_obj_center(lbl);

        lv_group_add_obj(g_group, item);
        lv_obj_add_event_cb(item, wifi_item_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
    }
}

/* ===== Display Page ===== */
void ui_display_create(lv_obj_t *scr)
{
    static const char *hints[] = {"←→调节", "B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, "显示设置", hints, 2);

    lv_obj_t *bright_lbl = lv_label_create(content);
    lv_label_set_text(bright_lbl, "屏幕亮度");
    lv_obj_set_style_text_color(bright_lbl, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_text_font(bright_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(bright_lbl, LV_ALIGN_TOP_MID, 0, 8);

    /* Slider */
    lv_obj_t *slider = lv_slider_create(content);
    lv_obj_set_size(slider, LCD_H_RES - 24, 12);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_CREAM), 0);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_BROWN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_BROWN), LV_PART_KNOB);

    lv_obj_t *val_lbl = lv_label_create(content);
    lv_label_set_text(val_lbl, "80%");
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, 20);

    lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *note = lv_label_create(content);
    lv_label_set_text(note, "* 背光引脚: GPIO0 (PWM)");
    lv_obj_set_style_text_color(note, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_10, 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 8, -4);
}

/* ===== About Page ===== */
void ui_about_create(lv_obj_t *scr)
{
    static const char *hints[] = {"B返回"};
    lv_obj_t *content = ui_create_standard_layout(scr, "关于本机", hints, 1);

    lv_obj_t *logo = lv_label_create(content);
    lv_label_set_text(logo, "🐱");
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_14, 0);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *name = lv_label_create(content);
    lv_label_set_text(name, "小喵桌面系统");
    lv_obj_set_style_text_color(name, lv_color_hex(UI_BLACK), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *ver = lv_label_create(content);
    lv_label_set_text(ver, "XiaoMiao Desktop OS v1.0.0");
    lv_obj_set_style_text_color(ver, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_10, 0);
    lv_obj_align(ver, LV_ALIGN_CENTER, 0, 14);

    /* Info grid */
    static const char *labels[] = {"芯片", "Flash", "PSRAM", "屏幕", "电池"};
    static const char *values[] = {"ESP32-WROVER-B", "4MB QIO 80MHz", "8MB Quad", "ST7735 160x128", "3.85V (ADC)"};

    for (int i = 0; i < 5; i++) {
        lv_obj_t *row = lv_label_create(content);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s: %s", labels[i], values[i]);
        lv_label_set_text(row, buf);
        lv_obj_set_style_text_color(row, lv_color_hex(UI_BLACK), 0);
        lv_obj_set_style_text_font(row, &lv_font_montserrat_10, 0);
        lv_obj_align(row, LV_ALIGN_LEFT_MID, 20, 30 + i * 14);
    }
}