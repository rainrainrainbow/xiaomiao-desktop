#ifndef XIAOMIAO_DESKTOP_H
#define XIAOMIAO_DESKTOP_H

#include "esp_err.h"
#include "lvgl.h"

/* ===== Hardware Constants ===== */
#define LCD_HOST            SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ  (60 * 1000 * 1000)
#define LCD_NATIVE_H_RES    128
#define LCD_NATIVE_V_RES    160
#define LCD_H_RES           160
#define LCD_V_RES           128
#define LCD_DPI             60

#define PIN_LCD_SCLK   GPIO_NUM_18
#define PIN_LCD_MOSI   GPIO_NUM_23
#define PIN_LCD_MISO   GPIO_NUM_19
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_DC     GPIO_NUM_4
#define PIN_SD_CS      GPIO_NUM_22

/* ===== Color Palette (Original Theme) ===== */
#define UI_YELLOW  0xF6D34A
#define UI_BLACK   0x1B1713
#define UI_BROWN   0x5C4220
#define UI_RED     0xE64B3C
#define UI_CREAM   0xFFF3B0
#define UI_GREEN   0x2DD466

/* ===== Screen Layout ===== */
#define TITLE_BAR_H    12
#define STATUS_BAR_H   10
#define HINT_BAR_H     10
#define CONTENT_Y      (TITLE_BAR_H + STATUS_BAR_H)
#define CONTENT_H      (LCD_V_RES - TITLE_BAR_H - STATUS_BAR_H - HINT_BAR_H)

/* ===== Keypad ===== */
#define PIN_BTN_UP     GPIO_NUM_2
#define PIN_BTN_DOWN   GPIO_NUM_13
#define PIN_BTN_LEFT   GPIO_NUM_27
#define PIN_BTN_RIGHT  GPIO_NUM_35
#define PIN_BTN_A      GPIO_NUM_34
#define PIN_BTN_B      GPIO_NUM_12
#define BUTTON_ACTIVE_LEVEL  0
#define BUTTON_DEBOUNCE_MS   25

/* ===== App Info ===== */
typedef struct {
    const char *id;
    const char *name;
    const char *icon;       /* Unicode emoji string */
    lv_color_t icon_bg;
    lv_obj_t *(*create_func)(lv_obj_t *parent);
} app_info_t;

/* ===== Global Exports ===== */
extern lv_display_t *g_display;
extern lv_group_t *g_group;
extern lv_indev_t *g_indev;
extern esp_lcd_panel_io_handle_t g_lcd_io;

/* ===== Page Navigation ===== */
typedef enum {
    PAGE_BOOT = 0,
    PAGE_DESKTOP,
    PAGE_TASKS,
    PAGE_SETTINGS,
    PAGE_WIFI,
    PAGE_DISPLAY,
    PAGE_ABOUT,
    PAGE_FILES,
    PAGE_APP_RUN,
    PAGE_COUNT
} page_id_t;

extern page_id_t g_current_page;
extern page_id_t g_prev_page;

void nav_to(page_id_t page);
void nav_back(void);

/* ===== UI Module Functions ===== */
void ui_main_init(void);
void ui_boot_create(lv_obj_t *scr);
void ui_desktop_create(lv_obj_t *scr);
void ui_tasks_create(lv_obj_t *scr);
void ui_settings_create(lv_obj_t *scr);
void ui_wifi_create(lv_obj_t *scr);
void ui_display_create(lv_obj_t *scr);
void ui_about_create(lv_obj_t *scr);
void ui_files_create(lv_obj_t *scr);
void ui_app_run_create(lv_obj_t *scr);

/* ===== Update Functions ===== */
void ui_update_time(void);
void ui_update_battery(void);
void ui_update_wifi_status(void);
void ui_update_task_count(void);

/* ===== External Modules ===== */
void keypad_init(void);
void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
void sd_card_init(void);
void task_manager_init(void);
void app_launcher_init(void);

#endif /* XIAOMIAO_DESKTOP_H */