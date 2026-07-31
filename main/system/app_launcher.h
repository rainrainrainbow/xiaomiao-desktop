#ifndef APP_LAUNCHER_H
#define APP_LAUNCHER_H

#include "lvgl.h"

/**
 * @brief Application entry point structure.
 * Each app provides a create function that builds its UI
 * on the provided parent object.
 */
typedef struct {
    const char *id;
    const char *name;
    const char *icon;       /* Unicode emoji */
    lv_color_t icon_bg;
    lv_obj_t *(*create)(lv_obj_t *parent);
} app_entry_t;

void app_launcher_init(void);
int app_launcher_get_count(void);
const app_entry_t *app_launcher_get_app(int index);
void app_launcher_launch(int index);

#endif /* APP_LAUNCHER_H */