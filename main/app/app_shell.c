/**
 * @file app_shell.c
 * @brief Shell 命令执行器应用（原 Python 应用改为 shell 终端）
 *
 * 交互：
 * - A 键：弹出虚拟键盘输入命令
 * - UP/DOWN：滚动查看输出
 * - B 键：返回桌面
 *
 * 支持命令：
 *   help / ls / cat / echo / clear / sysinfo
 *   brightness / ver / py / reboot
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "ui_keyboard.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/drv_backlight.h"
#include "app_micropython.h"
#include "poincare/runtime.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APP_SHELL";

/* ========== 输出缓冲区（静态） ========== */
#define SHELL_MAX_LINES 48
#define SHELL_LINE_LEN  64
static char s_lines[SHELL_MAX_LINES][SHELL_LINE_LEN];
static int  s_count = 0;
static int  s_scroll = 0;          /* 当前显示的第一行索引 */

static lv_obj_t *s_out_lbl = NULL;
static lv_obj_t *s_hint_lbl = NULL;

/* ========== 输出管理 ========== */
static void shell_render(void);

static void shell_append(const char *fmt, ...)
{
    char line[SHELL_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (s_count >= SHELL_MAX_LINES) {
        memmove(&s_lines[0], &s_lines[1], (SHELL_MAX_LINES - 1) * SHELL_LINE_LEN);
        s_count = SHELL_MAX_LINES - 1;
    }
    strncpy(s_lines[s_count], line, SHELL_LINE_LEN - 1);
    s_lines[s_count][SHELL_LINE_LEN - 1] = '\0';
    s_count++;
    if (s_count > 0) s_scroll = s_count - 1;
    shell_render();
}

static void shell_clear(void)
{
    s_count = 0;
    s_scroll = 0;
    shell_render();
}

static void shell_render(void)
{
    if (!s_out_lbl) return;
    ui_state_t *st = ui_state_get();
    int line_h = st->font_size + 3;
    if (line_h < 17) line_h = 17;
    int vis = (LCD_V_RES - ui_content_y() - DOCK_H - 6) / line_h;
    if (vis < 1) vis = 1;

    if (s_count == 0) {
        s_scroll = 0;
        lv_label_set_text(s_out_lbl, lang_get(STR_SHELL_WELCOME));
        if (s_hint_lbl) {
            char h[48];
            snprintf(h, sizeof(h), "A:%s  B:%s", lang_get(STR_SHELL_INPUT), lang_get(STR_BACK));
            lv_label_set_text(s_hint_lbl, h);
        }
        return;
    }
    /* s_scroll 为关注行索引（append 后指向最后一行）。渲染时对齐窗口： */
    int start = s_scroll;
    if (start + vis > s_count) start = s_count - vis;  /* 窗口底部对齐 */
    if (start < 0) start = 0;
    int end = start + vis;
    if (end > s_count) end = s_count;

    static char buf[SHELL_MAX_LINES * (SHELL_LINE_LEN + 1)];
    int pos = 0;
    for (int i = start; i < end && pos < (int)sizeof(buf) - 2; i++) {
        int l = (int)strlen(s_lines[i]);
        if (pos + l + 1 >= (int)sizeof(buf)) break;
        memcpy(buf + pos, s_lines[i], l);
        pos += l;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    lv_label_set_text(s_out_lbl, buf);

    if (s_hint_lbl) {
        char h[48];
        snprintf(h, sizeof(h), "A:%s %d/%d", lang_get(STR_SHELL_INPUT),
                 start + 1, s_count);
        lv_label_set_text(s_hint_lbl, h);
    }
}

/* ========== 命令实现 ========== */
static void cmd_help(void)
{
    shell_append("%s", lang_get(STR_SHELL_AVAIL));
    shell_append(" help ls cat echo clear");
    shell_append(" sysinfo brightness ver");
    shell_append(" py <%s> reboot", lang_get(STR_SHELL_CODE));
}

static void cmd_ls(const char *arg)
{
    const char *path = (arg && arg[0]) ? arg : "/sdcard";
    DIR *dir = opendir(path);
    if (!dir) {
        shell_append("ls: %s not found", path);
        return;
    }
    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(dir)) != NULL && n < 24) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type == DT_DIR) {
            shell_append(" [%s]/", ent->d_name);
        } else {
            shell_append(" %s", ent->d_name);
        }
        n++;
    }
    closedir(dir);
    if (n == 0) shell_append("(empty)");
    shell_append("%d entry", n);
}

static void cmd_cat(const char *arg)
{
    if (!arg || !arg[0]) {
        shell_append("cat: usage: cat <file>");
        return;
    }
    FILE *fp = fopen(arg, "r");
    if (!fp) {
        shell_append("cat: cannot open %s", arg);
        return;
    }
    char line[SHELL_LINE_LEN];
    int n = 0;
    while (fgets(line, sizeof(line), fp) && n < 20) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1]=='\n' || line[len-1]=='\r')) line[--len]='\0';
        shell_append("%s", line);
        n++;
    }
    fclose(fp);
}

static void cmd_echo(const char *arg)
{
    shell_append("%s", arg ? arg : "");
}

static void cmd_sysinfo(void)
{
    size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total8 = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    shell_append("ESP32-WROVER @240MHz");
    shell_append("DRAM: %lu/%luK", (unsigned long)(free8/1024), (unsigned long)(total8/1024));
    shell_append("PSRAM: %lu/%luK", (unsigned long)(free_ps/1024), (unsigned long)(total_ps/1024));
    shell_append("XiaoMiao %s", XIAOMIAO_VERSION);
}

static void cmd_brightness(const char *arg)
{
    if (!arg || !arg[0]) {
        shell_append("brightness: usage: brightness <0-100>");
        return;
    }
    int val = atoi(arg);
    if (val < 0) val = 0;
    if (val > 100) val = 100;
    drv_backlight_set_brightness(val);
    ui_state_t *st = ui_state_get();
    st->brightness = val;
    shell_append("brightness set to %d%%", val);
}

static void cmd_py(const char *arg)
{
    if (!arg || !arg[0]) {
        shell_append("py: usage: py <python code>");
        return;
    }
    shell_append("$ %s", arg);
    int ret = app_micropython_exec(arg, "<shell>");
    if (ret == 0) {
        shell_append("OK");
    } else {
        const char *err = poincare_runtime_get_last_error();
        shell_append("ERR: %s", (err && err[0]) ? err : "exec failed");
    }
}

/* 命令分发 */
static void shell_execute(const char *cmdline)
{
    if (!cmdline || !cmdline[0]) return;
    shell_append("$ %s", cmdline);

    char cmd[SHELL_LINE_LEN];
    char arg[SHELL_LINE_LEN];
    cmd[0] = '\0';
    arg[0] = '\0';

    const char *p = cmdline;
    while (*p == ' ') p++;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(cmd)-1) { cmd[i++] = *p++; }
    cmd[i] = '\0';
    while (*p == ' ') p++;
    strncpy(arg, p, sizeof(arg)-1);
    arg[sizeof(arg)-1] = '\0';

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) cmd_help();
    else if (strcmp(cmd, "ls") == 0) cmd_ls(arg);
    else if (strcmp(cmd, "cat") == 0) cmd_cat(arg);
    else if (strcmp(cmd, "echo") == 0) cmd_echo(arg);
    else if (strcmp(cmd, "clear") == 0) { shell_clear(); }
    else if (strcmp(cmd, "sysinfo") == 0) cmd_sysinfo();
    else if (strcmp(cmd, "brightness") == 0) cmd_brightness(arg);
    else if (strcmp(cmd, "ver") == 0) shell_append("XiaoMiao %s (%s)", XIAOMIAO_VERSION, XIAOMIAO_BUILD);
    else if (strcmp(cmd, "py") == 0) cmd_py(arg);
    else if (strcmp(cmd, "reboot") == 0) { shell_append("rebooting..."); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    else if (cmd[0] == '\0') { /* 空命令 */ }
    else shell_append("%s: %s (%s)", lang_get(STR_SHELL_UNKNOWN), cmd, lang_get(STR_SHELL_HELP));
}

/* ========== 键盘确认回调 ========== */
static void shell_kb_confirm(const char *text, void *user_data)
{
    (void)user_data;
    ui_keyboard_hide();
    shell_execute(text);
}

static void shell_kb_cancel(void *user_data)
{
    (void)user_data;
    ui_keyboard_hide();
}

/* ========== 页面生命周期 ========== */
static void shell_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Shell app init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_SHELL));

    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    /* 输出区 */
    s_out_lbl = lv_label_create(scr);
    if (!s_out_lbl) {
        ESP_LOGE(TAG, "lv_label_create(out) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lv_obj_t *container = lv_obj_create(scr);
        if (container) {
            lv_obj_remove_style_all(container);
            lv_obj_set_pos(container, 0, ui_content_y());
            lv_obj_set_size(container, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 6);
            lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
            /* 给 label 设置父对象为 container，但 label 自身承载文本 */
            lv_obj_set_parent(s_out_lbl, container);
            lv_obj_set_style_text_color(s_out_lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(s_out_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_bg_color(s_out_lbl, lv_color_hex(colors->bg), 0);
            lv_obj_set_style_bg_opa(s_out_lbl, LV_OPA_COVER, 0);
            lv_obj_set_pos(s_out_lbl, 3, 1);
            lv_obj_set_size(s_out_lbl, LCD_H_RES - 6, LCD_V_RES - ui_content_y() - DOCK_H - 6);
            lv_label_set_long_mode(s_out_lbl, LV_LABEL_LONG_WRAP);
            lv_label_set_text(s_out_lbl, "");
        }
    }

    /* 底部提示 */
    s_hint_lbl = lv_label_create(scr);
    if (s_hint_lbl) {
        lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(s_hint_lbl, lv_font_cn_get(12), 0);
        lv_obj_set_pos(s_hint_lbl, 2, LCD_V_RES - DOCK_H - 5);
        {
            char h[48];
            snprintf(h, sizeof(h), "A:%s  B:%s", lang_get(STR_SHELL_INPUT), lang_get(STR_BACK));
            lv_label_set_text(s_hint_lbl, h);
        }
    }

    ui_dock_create(scr, 1, 0);

    /* 初始化欢迎信息 */
    s_count = 0;
    s_scroll = 0;
    shell_append("XiaoMiao Shell");
    shell_append("type 'help' for commands");
}

static void shell_activate(void)
{
    ESP_LOGI(TAG, "Shell app activate");
    shell_render();
}

static void shell_destroy(void)
{
    ESP_LOGI(TAG, "Shell app destroy");
    if (ui_keyboard_is_visible()) ui_keyboard_hide();
    s_out_lbl = NULL;
    s_hint_lbl = NULL;
    s_count = 0;
    s_scroll = 0;
}

static bool shell_on_key(int key)
{
    /* 键盘可见时，按键交给键盘处理 */
    if (ui_keyboard_is_visible()) {
        return ui_keyboard_on_key(key);
    }

    switch (key) {
    case KEY_A:
        /* 弹出命令输入键盘 */
        {
            kb_config_t cfg = {
                .title = lang_get(STR_SHELL_TITLE),
                .placeholder = ">",
                .max_length = 60,
                .password_mode = false,
                .on_confirm = shell_kb_confirm,
                .on_cancel = shell_kb_cancel,
                .user_data = NULL,
            };
            ui_keyboard_show(&cfg);
        }
        return true;
    case KEY_UP:
        if (s_scroll > 0) { s_scroll--; shell_render(); }
        return true;
    case KEY_DOWN:
        if (s_scroll < s_count - 1) { s_scroll++; shell_render(); }
        return true;
    case KEY_B:
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    default:
        return false;
    }
}

const page_callbacks_t g_shell_callbacks = {
    .init = shell_init,
    .activate = shell_activate,
    .destroy = shell_destroy,
    .on_key = shell_on_key,
};