/*
 * mpy_engine.c — MicroPython Runtime Engine
 *
 * 基于 espressif/micropython IDF 官方组件。
 * 四层架构第四层：解释器生命周期、脚本执行、.app 包管理、
 * 硬件模块注册、C↔Python 桥接。
 *
 * 编译依赖：idf_component.yml 中声明
 *   dependencies:
 *     espressif/micropython: "^1.26.0"
 */

#include "mpy_engine.h"
#include "xiaomiao_desktop.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* MicroPython public C API from espressif/micropython component */
#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/objstr.h"
#include "py/objlist.h"
#include "py/objmodule.h"
#include "py/stream.h"
#include "py/smallint.h"
#include "py/nlr.h"
#include "py/scheduler.h"
#include "genhdr/mpversion.h"

static const char *TAG = "mpy_engine";

/* ===== External MicroPython symbols ===== */
/* mp_keyboard_interrupt_obj is defined in py/objexcept.c
 * and used by mp_sched_schedule to raise KeyboardInterrupt */
extern const mp_obj_type_t mp_keyboard_interrupt_obj;

/* ===== Engine State ===== */
static bool s_initialized = false;
static bool s_app_running = false;
static TaskHandle_t s_mpy_task = NULL;
static mpy_app_t s_current_app;
static lv_obj_t *s_app_container = NULL;
static void *s_mpy_heap = NULL;

/* ===== Callbacks ===== */
static mpy_key_cb_t s_key_cb = NULL;
static mpy_lcd_cb_t s_lcd_cb = NULL;

/* ===== Apps Directory ===== */
#define APPS_DIR "/sdcard/apps"

/* ========================================================================
 * 1. Engine Lifecycle
 * ======================================================================== */

esp_err_t mpy_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "MicroPython already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MicroPython %d.%d.%d initializing...",
             MICROPY_VERSION_MAJOR, MICROPY_VERSION_MINOR, MICROPY_VERSION_MICRO);

    /* API compatibility check for mp_compile_and_execute */
    #if MICROPY_VERSION_MAJOR < 1 || (MICROPY_VERSION_MAJOR == 1 && MICROPY_VERSION_MINOR < 20)
        #warning "MicroPython >= 1.20 required for mp_compile_and_execute API"
        ESP_LOGE(TAG, "MicroPython version too old: %d.%d.%d (need >= 1.20)",
                 MICROPY_VERSION_MAJOR, MICROPY_VERSION_MINOR, MICROPY_VERSION_MICRO);
        return ESP_ERR_INVALID_VERSION;
    #endif

    /* Step 1: Allocate GC heap from PSRAM (ESP32-WROVER-B: 8MB) */
    size_t heap_size = 256 * 1024; /* 256KB Python heap */
    s_mpy_heap = heap_caps_malloc(heap_size, MALLOC_CAP_SPIRAM);
    if (!s_mpy_heap) {
        ESP_LOGW(TAG, "PSRAM alloc failed, fallback to DRAM heap");
        s_mpy_heap = malloc(heap_size);
        if (!s_mpy_heap) {
            return ESP_ERR_NO_MEM;
        }
    }
    gc_init(s_mpy_heap, (uint8_t *)s_mpy_heap + heap_size);
    ESP_LOGI(TAG, "MicroPython GC heap: %u bytes", heap_size);

    /* Step 2: Initialize MicroPython core */
    mp_init();

    /* Step 3: Configure sys.path — use mp_obj_new_str for runtime strings */
    mp_obj_list_append(mp_sys_path, mp_obj_new_str("/sdcard/apps", 12));
    mp_obj_list_append(mp_sys_path, mp_obj_new_str("/sdcard/apps/lib", 16));
    mp_obj_list_append(mp_sys_path, mp_obj_new_str("/sdcard/lib", 11));

    /* Step 4: Ensure apps directory exists */
    struct stat st;
    if (stat(APPS_DIR, &st) != 0) {
        mkdir(APPS_DIR, 0777);
        ESP_LOGI(TAG, "Created apps directory: %s", APPS_DIR);
    }

    /* Step 5: Register xiaomiao hardware modules */
    mpy_register_hardware_modules();

    s_initialized = true;
    ESP_LOGI(TAG, "MicroPython engine initialized successfully");
    return ESP_OK;
}

esp_err_t mpy_deinit(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (s_app_running) mpy_stop_app();

    mp_deinit();

    if (s_mpy_heap) {
        free(s_mpy_heap);
        s_mpy_heap = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "MicroPython engine deinitialized");
    return ESP_OK;
}

bool mpy_is_running(void)
{
    return s_app_running;
}

/* ========================================================================
 * 2. Script Execution
 * ======================================================================== */

esp_err_t mpy_exec(const char *script)
{
    if (!script || !s_initialized) return ESP_ERR_INVALID_STATE;

    size_t len = strlen(script);
    ESP_LOGI(TAG, "Executing script (%u bytes)", len);

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_compile_and_execute(script, len, MP_PARSE_FILE_INPUT);
        nlr_pop();
        ESP_LOGI(TAG, "Script executed successfully");
        return ESP_OK;
    } else {
        mp_obj_t exc = (mp_obj_t)nlr.ret_val;
        mp_obj_print_exception(&mp_plat_print, exc);
        ESP_LOGE(TAG, "Script execution failed");
        return ESP_FAIL;
    }
}

esp_err_t mpy_exec_file(const char *path)
{
    if (!path || !s_initialized) return ESP_ERR_INVALID_STATE;

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc(MPY_MAX_SCRIPT_SIZE);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t bytes = fread(buf, 1, MPY_MAX_SCRIPT_SIZE - 1, f);
    fclose(f);
    buf[bytes] = '\0';

    esp_err_t err = mpy_exec(buf);
    free(buf);
    return err;
}

/* ========================================================================
 * 3. App Package Scanning
 * ======================================================================== */

int mpy_scan_apps(const char *dir, mpy_app_t *apps, int max_apps)
{
    if (!dir || !apps || max_apps <= 0) return 0;

    DIR *d = opendir(dir);
    if (!d) { ESP_LOGW(TAG, "Cannot open: %s", dir); return 0; }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_apps) {
        if (entry->d_name[0] == '.') continue;

        char full_path[MPY_APP_PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* Must end with .app */
        size_t nl = strlen(entry->d_name);
        if (nl < 4 || strcmp(entry->d_name + nl - 4, ".app") != 0) continue;

        /* Check manifest.json */
        char mp[MPY_APP_PATH_MAX + 16];
        snprintf(mp, sizeof(mp), "%s/manifest.json", full_path);
        if (stat(mp, &st) != 0) continue;

        FILE *f = fopen(mp, "r");
        if (!f) continue;
        char mb[2048];
        size_t br = fread(mb, 1, sizeof(mb) - 1, f);
        fclose(f);
        mb[br] = '\0';

        mpy_app_t *app = &apps[count];
        memset(app, 0, sizeof(mpy_app_t));

        if (mpy_parse_manifest(mb, app) == ESP_OK) {
            strncpy(app->path, full_path, sizeof(app->path) - 1);
            char ip[MPY_APP_PATH_MAX + 16];
            snprintf(ip, sizeof(ip), "%s/icon.png", full_path);
            app->has_icon = (stat(ip, &st) == 0);
            char mp2[MPY_APP_PATH_MAX + 16];
            snprintf(mp2, sizeof(mp2), "%s/main.py", full_path);
            if (stat(mp2, &st) == 0) {
                count++;
                ESP_LOGI(TAG, "Found app: %s v%s (%s)", app->name, app->version, app->id);
            }
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "Scanned %d apps", count);
    return count;
}

/* ========================================================================
 * 4. Manifest Parser (lightweight JSON extractor)
 * ======================================================================== */

esp_err_t mpy_parse_manifest(const char *json_str, mpy_app_t *app)
{
    if (!json_str || !app) return ESP_ERR_INVALID_ARG;

    #define EXTRACT_FIELD(fn, dest, maxlen) do { \
        const char *_p = strstr(json_str, "\"" fn "\""); \
        if (_p) { \
            _p = strchr(_p, ':'); \
            if (_p) { _p = strchr(_p, '"'); \
                if (_p) { _p++; \
                    const char *_e = strchr(_p, '"'); \
                    if (_e) { \
                        size_t _l = (_e - _p) < (maxlen-1) ? (_e - _p) : (maxlen-1); \
                        strncpy(dest, _p, _l); dest[_l] = '\0'; \
                    } \
                } \
            } \
        } \
    } while(0)

    EXTRACT_FIELD("id", app->id, sizeof(app->id));
    EXTRACT_FIELD("name", app->name, sizeof(app->name));
    EXTRACT_FIELD("version", app->version, sizeof(app->version));
    EXTRACT_FIELD("author", app->author, sizeof(app->author));
    EXTRACT_FIELD("icon_emoji", app->icon_emoji, sizeof(app->icon_emoji));
    #undef EXTRACT_FIELD

    if (strlen(app->id) == 0) {
        ESP_LOGW(TAG, "Manifest missing 'id'");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(app->name) == 0)
        strncpy(app->name, app->id, sizeof(app->name) - 1);
    if (strlen(app->icon_emoji) == 0)
        strncpy(app->icon_emoji, "📦", sizeof(app->icon_emoji) - 1);

    return ESP_OK;
}

esp_err_t mpy_load_app_icon(const mpy_app_t *app, uint8_t *buf, size_t max, size_t *out)
{
    if (!app || !buf || !out) return ESP_ERR_INVALID_ARG;
    char ip[MPY_APP_PATH_MAX + 16];
    snprintf(ip, sizeof(ip), "%s/icon.png", app->path);
    FILE *f = fopen(ip, "r");
    if (!f) { *out = 0; return ESP_ERR_NOT_FOUND; }
    *out = fread(buf, 1, max, f);
    fclose(f);
    return ESP_OK;
}

/* ========================================================================
 * 5. App Launch / Stop
 * ======================================================================== */

static void mpy_app_task(void *arg)
{
    const mpy_app_t *app = (const mpy_app_t *)arg;
    ESP_LOGI(TAG, "Starting app: %s (%s)", app->name, app->id);

    chdir(s_current_app.path);
    mp_obj_list_append(mp_sys_path, mp_obj_new_str(s_current_app.path, strlen(s_current_app.path)));

    char main_path[MPY_APP_PATH_MAX + 16];
    snprintf(main_path, sizeof(main_path), "%s/main.py", s_current_app.path);

    esp_err_t err = mpy_exec_file(main_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "App %s failed", app->id);
        /* Show error via lv_async_call — safe from any task context */
        if (s_app_container) {
            lv_async_call((lv_async_cb_t)lv_obj_clean, s_app_container);
            lv_async_call((lv_async_cb_t)mpy_show_error, s_app_container);
        }
    }

    s_app_running = false;
    ESP_LOGI(TAG, "App %s stopped", app->id);

    if (g_current_page == PAGE_APP_RUN) {
        lv_async_call((lv_async_cb_t)nav_to, (void *)(uintptr_t)PAGE_DESKTOP);
    }

    vTaskDelete(NULL);
}

/* Helper called via lv_async_call to show error on LVGL container */
static void mpy_show_error(lv_obj_t *container)
{
    lv_obj_t *el = lv_label_create(container);
    lv_label_set_text(el, "应用加载失败");
    lv_obj_set_style_text_color(el, lv_color_hex(UI_RED), 0);
    lv_obj_center(el);
}

esp_err_t mpy_launch_app(lv_obj_t *container, const mpy_app_t *app)
{
    if (!container || !app || s_app_running) return ESP_ERR_INVALID_STATE;

    s_app_container = container;
    memcpy(&s_current_app, app, sizeof(mpy_app_t));
    s_app_running = true;

    lv_obj_clean(container);
    lv_obj_t *loading = lv_label_create(container);
    lv_label_set_text(loading, "加载中...");
    lv_obj_set_style_text_color(loading, lv_color_hex(UI_BLACK), 0);
    lv_obj_center(loading);

    BaseType_t ret = xTaskCreate(
        mpy_app_task, "mpy_app", MPY_APP_TASK_STACK,
        &s_current_app,  /* Pass static copy, not caller's stack pointer */
        3, &s_mpy_task
    );

    if (ret != pdPASS) {
        s_app_running = false;
        ESP_LOGE(TAG, "Failed to create MicroPython task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "App '%s' launched", app->name);
    return ESP_OK;
}

void mpy_stop_app(void)
{
    if (!s_app_running) return;
    ESP_LOGI(TAG, "Stopping current app...");

    /* Signal the MicroPython task to stop.
     * Schedule a KeyboardInterrupt via mp_sched_schedule.
     * This will cause the running Python code to raise an exception. */
    #if MICROPY_ENABLE_SCHEDULER
        mp_sched_schedule(MP_OBJ_FROM_PTR(&mp_keyboard_interrupt_obj), mp_const_none);
    #endif

    if (s_mpy_task) {
        TickType_t start = xTaskGetTickCount();
        while (s_app_running && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(3000)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_app_running) {
            ESP_LOGW(TAG, "App did not stop gracefully, force deleting task");
            vTaskDelete(s_mpy_task);
        }
        s_mpy_task = NULL;
    }

    s_app_running = false;
    /* Clean LVGL container via async call */
    if (s_app_container) {
        lv_async_call((lv_async_cb_t)lv_obj_clean, s_app_container);
    }
    ESP_LOGI(TAG, "App stopped");
}

/* ========================================================================
 * 6. Event Processing
 * ======================================================================== */

void mpy_process_events(void)
{
    /* Only process events when MicroPython is NOT running an app.
     * The interpreter is not reentrant — calling mp_handle_pending
     * while mpy_app_task is executing would cause undefined behavior. */
    #if MICROPY_ENABLE_SCHEDULER
        if (s_initialized && !s_app_running) {
            mp_handle_pending(true);
        }
    #endif
}

/* ========================================================================
 * 7. Callback Registration
 * ======================================================================== */

void mpy_set_key_callback(mpy_key_cb_t cb) { s_key_cb = cb; }
void mpy_set_lcd_callback(mpy_lcd_cb_t cb) { s_lcd_cb = cb; }

/* ========================================================================
 * 8. Hardware Module Registration
 * ======================================================================== */

/* ---- LCD ---- */
static mp_obj_t py_lcd_fill(mp_obj_t color_obj) {
    (void)color_obj; if (s_lcd_cb) {} return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(py_lcd_fill_obj, py_lcd_fill);

static mp_obj_t py_lcd_pixel(mp_obj_t x, mp_obj_t y, mp_obj_t c) {
    (void)x; (void)y; (void)c; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_3(py_lcd_pixel_obj, py_lcd_pixel);

static const mp_rom_map_elem_t lcd_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_fill),   MP_ROM_PTR(&py_lcd_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel),  MP_ROM_PTR(&py_lcd_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_WIDTH),  MP_ROM_INT(LCD_H_RES) },
    { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(LCD_V_RES) },
};
static MP_DEFINE_CONST_DICT(lcd_dict, lcd_tbl);
static const mp_obj_module_t lcd_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&lcd_dict,
};

/* ---- Keypad ---- */
static mp_obj_t py_key_get(mp_obj_t timeout_ms) {
    (void)timeout_ms; return mp_obj_new_int(-1); }
static MP_DEFINE_CONST_FUN_OBJ_1(py_key_get_obj, py_key_get);

static mp_obj_t py_key_is_pressed(mp_obj_t key) {
    (void)key; return mp_const_false; }
static MP_DEFINE_CONST_FUN_OBJ_1(py_key_is_pressed_obj, py_key_is_pressed);

static const mp_rom_map_elem_t key_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_get),        MP_ROM_PTR(&py_key_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_pressed), MP_ROM_PTR(&py_key_is_pressed_obj) },
    { MP_ROM_QSTR(MP_QSTR_UP),    MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_DOWN),  MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_LEFT),  MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_RIGHT), MP_ROM_INT(3) },
    { MP_ROM_QSTR(MP_QSTR_A),     MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_B),     MP_ROM_INT(5) },
};
static MP_DEFINE_CONST_DICT(key_dict, key_tbl);
static const mp_obj_module_t key_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&key_dict,
};

/* ---- Buzzer ---- */
static mp_obj_t py_buz_tone(mp_obj_t freq, mp_obj_t dur) {
    (void)freq; (void)dur; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_2(py_buz_tone_obj, py_buz_tone);
static mp_obj_t py_buz_off(void) { return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(py_buz_off_obj, py_buz_off);

static const mp_rom_map_elem_t buz_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_tone), MP_ROM_PTR(&py_buz_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_off),  MP_ROM_PTR(&py_buz_off_obj) },
};
static MP_DEFINE_CONST_DICT(buz_dict, buz_tbl);
static const mp_obj_module_t buz_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&buz_dict,
};

/* ---- LED (GD32 via I2C) ---- */
static mp_obj_t py_led_set(mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    (void)r; (void)g; (void)b; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_3(py_led_set_obj, py_led_set);
static const mp_rom_map_elem_t led_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&py_led_set_obj) },
};
static MP_DEFINE_CONST_DICT(led_dict, led_tbl);
static const mp_obj_module_t led_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&led_dict,
};

/* ---- Motor ---- */
static mp_obj_t py_motor_speed(mp_obj_t s) {
    (void)s; return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_1(py_motor_speed_obj, py_motor_speed);
static mp_obj_t py_motor_stop(void) { return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(py_motor_stop_obj, py_motor_stop);
static const mp_rom_map_elem_t motor_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_set_speed), MP_ROM_PTR(&py_motor_speed_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),      MP_ROM_PTR(&py_motor_stop_obj) },
};
static MP_DEFINE_CONST_DICT(motor_dict, motor_tbl);
static const mp_obj_module_t motor_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&motor_dict,
};

/* ---- Battery ---- */
static mp_obj_t py_bat_read(void) {
    /* ADC1_CH6 = GPIO34 */
    return mp_obj_new_int(85); }
static MP_DEFINE_CONST_FUN_OBJ_0(py_bat_read_obj, py_bat_read);
static const mp_rom_map_elem_t bat_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_read),  MP_ROM_PTR(&py_bat_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_level), MP_ROM_INT(85) },
};
static MP_DEFINE_CONST_DICT(bat_dict, bat_tbl);
static const mp_obj_module_t bat_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&bat_dict,
};

/* ---- SD Card ---- */
static mp_obj_t py_sd_listdir(mp_obj_t p) {
    const char *path = mp_obj_str_get_str(p);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            mp_obj_list_append(list, mp_obj_new_str(e->d_name, strlen(e->d_name)));
        }
        closedir(d);
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_sd_listdir_obj, py_sd_listdir);

static mp_obj_t py_sd_exists(mp_obj_t p) {
    struct stat st;
    return mp_obj_new_bool(stat(mp_obj_str_get_str(p), &st) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_sd_exists_obj, py_sd_exists);

static const mp_rom_map_elem_t sd_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_listdir),     MP_ROM_PTR(&py_sd_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_file_exists), MP_ROM_PTR(&py_sd_exists_obj) },
};
static MP_DEFINE_CONST_DICT(sd_dict, sd_tbl);
static const mp_obj_module_t sd_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&sd_dict,
};

/* ---- LVGL Draw — all functions use async dispatch to LVGL task ---- */
static mp_obj_t py_lv_clear(void) {
    if (s_app_container) {
        lv_async_call((lv_async_cb_t)lv_obj_clean, s_app_container);
    }
    return mp_const_none; }
static MP_DEFINE_CONST_FUN_OBJ_0(py_lv_clear_obj, py_lv_clear);

/* LVGL object creation data passed via async callback */
typedef struct {
    lv_obj_t *container;
    char text[32];
    int16_t x, y;
} lv_label_data_t;

static void lv_async_create_label(void *data)
{
    lv_label_data_t *d = (lv_label_data_t *)data;
    lv_obj_t *l = lv_label_create(d->container);
    lv_label_set_text(l, d->text);
    lv_obj_set_pos(l, d->x, d->y);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_BLACK), 0);
    free(data);
}

static mp_obj_t py_lv_label(mp_obj_t t, mp_obj_t x, mp_obj_t y) {
    if (s_app_container) {
        lv_label_data_t *d = malloc(sizeof(lv_label_data_t));
        if (d) {
            d->container = s_app_container;
            strncpy(d->text, mp_obj_str_get_str(t), sizeof(d->text) - 1);
            d->text[sizeof(d->text) - 1] = '\0';
            d->x = mp_obj_get_int(x);
            d->y = mp_obj_get_int(y);
            lv_async_call(lv_async_create_label, d);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(py_lv_label_obj, py_lv_label);

typedef struct {
    lv_obj_t *container;
    int16_t x, y, w, h;
    uint32_t color;
} lv_rect_data_t;

static void lv_async_create_rect(void *data)
{
    lv_rect_data_t *d = (lv_rect_data_t *)data;
    lv_obj_t *r = lv_obj_create(d->container);
    lv_obj_set_size(r, d->w, d->h);
    lv_obj_set_pos(r, d->x, d->y);
    lv_obj_set_style_bg_color(r, lv_color_hex(d->color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    free(data);
}

static mp_obj_t py_lv_rect(mp_obj_t x, mp_obj_t y, mp_obj_t w, mp_obj_t h, mp_obj_t c) {
    if (s_app_container) {
        lv_rect_data_t *d = malloc(sizeof(lv_rect_data_t));
        if (d) {
            d->container = s_app_container;
            d->x = mp_obj_get_int(x);
            d->y = mp_obj_get_int(y);
            d->w = mp_obj_get_int(w);
            d->h = mp_obj_get_int(h);
            d->color = mp_obj_get_int(c);
            lv_async_call(lv_async_create_rect, d);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_5(py_lv_rect_obj, py_lv_rect);

static const mp_rom_map_elem_t lv_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&py_lv_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&py_lv_label_obj) },
    { MP_ROM_QSTR(MP_QSTR_rect),  MP_ROM_PTR(&py_lv_rect_obj) },
};
static MP_DEFINE_CONST_DICT(lv_dict, lv_tbl);
static const mp_obj_module_t lv_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&lv_dict,
};

/* ---- Time ---- */
static mp_obj_t py_xm_sleep(mp_obj_t ms) {
    vTaskDelay(pdMS_TO_TICKS(mp_obj_get_int(ms)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(py_xm_sleep_obj, py_xm_sleep);

static const mp_rom_map_elem_t time_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR_msleep), MP_ROM_PTR(&py_xm_sleep_obj) },
};
static MP_DEFINE_CONST_DICT(time_dict, time_tbl);
static const mp_obj_module_t time_mod = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&time_dict,
};

/* ---- xiaomiao Top-Level Module ---- */
static const mp_rom_map_elem_t xm_module_tbl[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_xiaomiao) },
    { MP_ROM_QSTR(MP_QSTR_lcd),      MP_ROM_PTR(&lcd_mod) },
    { MP_ROM_QSTR(MP_QSTR_keypad),   MP_ROM_PTR(&key_mod) },
    { MP_ROM_QSTR(MP_QSTR_buzzer),   MP_ROM_PTR(&buz_mod) },
    { MP_ROM_QSTR(MP_QSTR_led),      MP_ROM_PTR(&led_mod) },
    { MP_ROM_QSTR(MP_QSTR_motor),    MP_ROM_PTR(&motor_mod) },
    { MP_ROM_QSTR(MP_QSTR_battery),  MP_ROM_PTR(&bat_mod) },
    { MP_ROM_QSTR(MP_QSTR_sd),       MP_ROM_PTR(&sd_mod) },
    { MP_ROM_QSTR(MP_QSTR_lvgl),     MP_ROM_PTR(&lv_mod) },
    { MP_ROM_QSTR(MP_QSTR_time),     MP_ROM_PTR(&time_mod) },
};
static MP_DEFINE_CONST_DICT(xm_module_dict, xm_module_tbl);
static const mp_obj_module_t xm_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&xm_module_dict,
};

esp_err_t mpy_register_hardware_modules(void)
{
    ESP_LOGI(TAG, "Registering xiaomiao hardware modules...");

    mp_store_name(MP_QSTR_xiaomiao, MP_OBJ_FROM_PTR(&xm_module));
    mp_store_name(MP_QSTR_lcd,      MP_OBJ_FROM_PTR(&lcd_mod));
    mp_store_name(MP_QSTR_keypad,   MP_OBJ_FROM_PTR(&key_mod));
    mp_store_name(MP_QSTR_buzzer,   MP_OBJ_FROM_PTR(&buz_mod));
    mp_store_name(MP_QSTR_battery,  MP_OBJ_FROM_PTR(&bat_mod));
    mp_store_name(MP_QSTR_lvgl,     MP_OBJ_FROM_PTR(&lv_mod));
    mp_store_name(MP_QSTR_xm_time,  MP_OBJ_FROM_PTR(&time_mod));

    ESP_LOGI(TAG, "Hardware modules registered: lcd, keypad, buzzer, led, motor, battery, sd, lvgl, time");
    return ESP_OK;
}