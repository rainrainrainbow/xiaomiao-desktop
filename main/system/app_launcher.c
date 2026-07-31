#include "app_launcher.h"
#include "xiaomiao_desktop.h"
#include "task_manager.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app_launcher";

/* Forward declarations of built-in app create functions */
extern lv_obj_t *snake_create(lv_obj_t *parent);
extern lv_obj_t *calc_create(lv_obj_t *parent);
extern lv_obj_t *music_player_create(lv_obj_t *parent);

/* App entries — icon_bg initialized at runtime via app_launcher_init */
static app_entry_t g_builtin_apps[6];

#define APP_COUNT (sizeof(g_builtin_apps) / sizeof(g_builtin_apps[0]))

void app_launcher_init(void)
{
    /* Initialize app entries at runtime (lv_color_hex is not constexpr) */
    g_builtin_apps[0] = (app_entry_t){ .id = "settings", .name = "设置",  .icon = "⚙️", .icon_bg = lv_color_hex(0x667eea), .create = NULL };
    g_builtin_apps[1] = (app_entry_t){ .id = "files",    .name = "文件",  .icon = "📁", .icon_bg = lv_color_hex(0xf093fb), .create = NULL };
    g_builtin_apps[2] = (app_entry_t){ .id = "game",     .name = "贪吃蛇",.icon = "🐍", .icon_bg = lv_color_hex(0x4facfe), .create = snake_create };
    g_builtin_apps[3] = (app_entry_t){ .id = "music",    .name = "音乐",  .icon = "🎵", .icon_bg = lv_color_hex(0x43e97b), .create = music_player_create };
    g_builtin_apps[4] = (app_entry_t){ .id = "calc",     .name = "计算器",.icon = "🔢", .icon_bg = lv_color_hex(0xfa709a), .create = calc_create };
    g_builtin_apps[5] = (app_entry_t){ .id = "tasks",    .name = "任务",  .icon = "📋", .icon_bg = lv_color_hex(0xff9a9e), .create = NULL };

    ESP_LOGI(TAG, "App launcher initialized with %d apps", APP_COUNT);
}

int app_launcher_get_count(void)
{
    return APP_COUNT;
}

const app_entry_t *app_launcher_get_app(int index)
{
    if (index < 0 || index >= (int)APP_COUNT) return NULL;
    return &g_builtin_apps[index];
}

void app_launcher_launch(int index)
{
    if (index < 0 || index >= (int)APP_COUNT) return;

    const app_entry_t *app = &g_builtin_apps[index];

    if (app->create != NULL) {
        /* Launch a built-in app - add to task manager and show app page */
        task_manager_add_task(app->id, app->name, app->icon, app->icon_bg, app->create);
        nav_to(PAGE_APP_RUN);
    } else {
        /* Special system pages */
        if (strcmp(app->id, "settings") == 0) {
            nav_to(PAGE_SETTINGS);
        } else if (strcmp(app->id, "files") == 0) {
            nav_to(PAGE_FILES);
        } else if (strcmp(app->id, "tasks") == 0) {
            nav_to(PAGE_TASKS);
        }
    }
}