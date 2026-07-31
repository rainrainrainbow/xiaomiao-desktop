#ifndef UI_WIDGETS_H
#define UI_WIDGETS_H

#include "lvgl.h"
#include "xiaomiao_desktop.h"

/**
 * @brief Create a standard screen layout with title bar, status bar, content area, and hint bar.
 * 
 * @param parent The screen object
 * @param title Title text for the title bar
 * @param hint_items Array of hint item strings, e.g. {"↑↓选择","A打开","B返回"}
 * @param hint_count Number of hint items
 * @return lv_obj_t* The content area (for adding main content)
 */
lv_obj_t *ui_create_standard_layout(lv_obj_t *parent, const char *title,
                                    const char *hint_items[], int hint_count);

/**
 * @brief Update the time display on the status bar.
 */
void ui_update_time(void);

/**
 * @brief Update the battery level display on the status bar.
 */
void ui_update_battery(void);

/**
 * @brief Update the WiFi status icon on the status bar.
 */
void ui_update_wifi_status(void);

/**
 * @brief Show a toast message.
 */
void ui_show_toast(const char *msg);

/* Status bar labels (set by ui_create_standard_layout) */
extern lv_obj_t *g_status_time;
extern lv_obj_t *g_status_wifi;
extern lv_obj_t *g_status_battery;

#endif /* UI_WIDGETS_H */