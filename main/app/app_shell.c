/**
 * @file app_shell.c
 * @brief Shell 命令执行器应用（原 Python 应用改为 shell 终端）
 *
 * 交互（5 键）：
 * - 输出视图（默认）：
 *   A         — 弹出虚拟键盘输入新命令
 *   UP/DOWN   — 滚动查看输出
 *   LEFT/RIGHT— 切换到「历史视图」
 *   B         — 返回桌面
 * - 历史视图（LEFT/RIGHT 进入）：
 *   UP/DOWN   — 在最近命令列表中移动选择（UP=更早，DOWN=更新）
 *   A         — 直接重新执行选中的历史命令
 *   RIGHT     — 把选中命令预填进输入框（可继续追加/退格编辑），确认后执行
 *   LEFT / B  — 返回输出视图
 * - 虚拟键盘：A 输入字符，B 退格（无文本时取消），方向键移动字符网格，
 *   网格 OK 键提交。键盘本身不支持光标移动，故编辑方式为「回填 + 追加/退格」。
 *
 * 支持命令：
 *   help / ls / cat / echo / clear / sysinfo
 *   mem / bat / sd / brightness / ver / py / reboot
 *
 * 历史：最近 16 条命令环形缓存在内部 RAM（static ~1KB），通过 NVS blob
 *   键 "shell_hist" 持久化，启动加载、提交命令时保存。
 */
#include "app_sys_api.h"   /* 系统状态 / 配置持久化门面层 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "ui_keyboard.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include "system/sys_nvs.h"   /* NVS_NAMESPACE：与系统设置共用同一命名空间 */
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
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
/* 纯 UI shell 文本缓存，放 PSRAM：3KB+3KB 内部 DRAM 释放 */
EXT_RAM_BSS_ATTR static char s_lines[SHELL_MAX_LINES][SHELL_LINE_LEN];
static int  s_count = 0;
static int  s_scroll = 0;          /* 当前显示的第一行索引 */

/* 输出/历史视图共用渲染缓冲（PSRAM，避免两个静态 buf 各占 3KB） */
EXT_RAM_BSS_ATTR static char s_render_buf[SHELL_MAX_LINES * (SHELL_LINE_LEN + 1)];

static lv_obj_t *s_out_lbl = NULL;
static lv_obj_t *s_hint_lbl = NULL;

/* ========== 命令历史（环形缓冲，内部 RAM 静态 ~1KB） ========== */
#define SHELL_HIST_MAX     16
#define SHELL_HIST_LEN     64
#define SHELL_HIST_NVS_KEY "shell_hist"

typedef struct {
    int  count;                              /* 已保存命令数 0..SHELL_HIST_MAX */
    int  head;                               /* 下一条写入位置 0..SHELL_HIST_MAX-1 */
    char items[SHELL_HIST_MAX][SHELL_HIST_LEN];
} shell_hist_t;

static shell_hist_t s_hist;                  /* 不用栈上大数组，整体 static */
static int s_view = 0;                       /* 0=输出视图 1=历史视图 */
static int s_hist_sel = 0;                   /* 历史视图选中项（逻辑索引 0..count-1） */

#define SHELL_VIEW_OUTPUT 0
#define SHELL_VIEW_HIST   1

/* ========== 输出管理 ========== */
static void shell_render(void);

/* ---------- 历史持久化：NVS blob ---------- */
static void shell_hist_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, SHELL_HIST_NVS_KEY, &s_hist, sizeof(s_hist));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "history nvs_set_blob failed: %s", esp_err_to_name(err));
    } else {
        err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGE(TAG, "history nvs_commit failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "history saved (%d cmds)", s_hist.count);
}

static void shell_hist_load(void)
{
    memset(&s_hist, 0, sizeof(s_hist));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "history nvs_open(RO) failed: %s", esp_err_to_name(err));
        return;
    }
    size_t len = sizeof(s_hist);
    err = nvs_get_blob(h, SHELL_HIST_NVS_KEY, &s_hist, &len);
    if (err == ESP_OK && len == sizeof(s_hist)) {
        /* 校验范围，防止旧版本/损坏数据导致越界 */
        if (s_hist.count >= 0 && s_hist.count <= SHELL_HIST_MAX &&
            s_hist.head >= 0 && s_hist.head < SHELL_HIST_MAX) {
            for (int i = 0; i < SHELL_HIST_MAX; i++) {
                s_hist.items[i][SHELL_HIST_LEN - 1] = '\0';
            }
            ESP_LOGI(TAG, "history loaded (%d cmds)", s_hist.count);
        } else {
            ESP_LOGW(TAG, "history data invalid, reset");
            memset(&s_hist, 0, sizeof(s_hist));
        }
    } else {
        ESP_LOGI(TAG, "no history in NVS (err=%s, len=%u)",
                 esp_err_to_name(err), (unsigned)len);
    }
    nvs_close(h);
}

/* 逻辑索引 i（0=最旧, count-1=最新）映射到环形缓冲物理槽位 */
static char *shell_hist_item(int idx)
{
    if (idx < 0 || idx >= s_hist.count) return NULL;
    int phys = (s_hist.head - s_hist.count + idx) % SHELL_HIST_MAX;
    if (phys < 0) phys += SHELL_HIST_MAX;
    return s_hist.items[phys];
}

/* 提交命令时记录历史：去首尾空白、跳过空串与连续重复，再写环形缓冲并落盘 */
static void shell_hist_add(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    const char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;

    if (s_hist.count > 0) {
        char *last = shell_hist_item(s_hist.count - 1);
        if (last && strncmp(last, p, SHELL_HIST_LEN) == 0) return;
    }

    char *slot = s_hist.items[s_hist.head];
    strncpy(slot, p, SHELL_HIST_LEN - 1);
    slot[SHELL_HIST_LEN - 1] = '\0';
    s_hist.head = (s_hist.head + 1) % SHELL_HIST_MAX;
    if (s_hist.count < SHELL_HIST_MAX) s_hist.count++;
    shell_hist_save();
}

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
    int vis = (LCD_V_RES - ui_content_y() - DOCK_H - 16) / line_h;
    if (vis < 1) vis = 1;

    if (s_count == 0) {
        s_scroll = 0;
        lv_label_set_text(s_out_lbl, lang_get(STR_SHELL_WELCOME));
        if (s_hint_lbl) {
            char h[96];
            snprintf(h, sizeof(h), "A:%s  <:%s  B:%s", lang_get(STR_SHELL_INPUT),
                     lang_get(STR_SHELL_HISTORY), lang_get(STR_COMMON_BACK));
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

    int pos = 0;
    for (int i = start; i < end && pos < (int)sizeof(s_render_buf) - 2; i++) {
        int l = (int)strlen(s_lines[i]);
        if (pos + l + 1 >= (int)sizeof(s_render_buf)) break;
        memcpy(s_render_buf + pos, s_lines[i], l);
        pos += l;
        s_render_buf[pos++] = '\n';
    }
    s_render_buf[pos] = '\0';
    lv_label_set_text(s_out_lbl, s_render_buf);

    if (s_hint_lbl) {
        char h[96];
        snprintf(h, sizeof(h), "A:%s <:%s %d/%d", lang_get(STR_SHELL_INPUT),
                 lang_get(STR_SHELL_HISTORY), start + 1, s_count);
        lv_label_set_text(s_hint_lbl, h);
    }
}

/* ========== 历史视图渲染 ========== */
static void shell_render_history(void)
{
    if (!s_out_lbl) return;
    ui_state_t *st = ui_state_get();
    int line_h = st->font_size + 3;
    if (line_h < 17) line_h = 17;
    int vis = (LCD_V_RES - ui_content_y() - DOCK_H - 16) / line_h;
    if (vis < 1) vis = 1;

    int pos = 0;
    int n;

    if (s_hist.count == 0) {
        n = snprintf(s_render_buf, sizeof(s_render_buf), "%s\n(empty)\n",
                     lang_get(STR_SHELL_HISTORY));
        if (n > 0) pos = n < (int)sizeof(s_render_buf) ? n : (int)sizeof(s_render_buf) - 1;
    } else {
        if (s_hist_sel < 0) s_hist_sel = 0;
        if (s_hist_sel >= s_hist.count) s_hist_sel = s_hist.count - 1;
        /* 窗口保证选中项可见；默认选中最新一条（底部），符合 shell 习惯 */
        int start = s_hist_sel - vis + 1;
        if (start < 0) start = 0;
        if (start > s_hist.count - vis) start = s_hist.count - vis;
        if (start < 0) start = 0;
        int end = start + vis;
        if (end > s_hist.count) end = s_hist.count;

        for (int i = start; i < end; i++) {
            const char *mark = (i == s_hist_sel) ? "> " : "  ";
            char *item = shell_hist_item(i);
            n = snprintf(s_render_buf + pos, sizeof(s_render_buf) - pos, "%s%d. %s\n",
                         mark, i + 1, item ? item : "");
            if (n < 0 || pos + n >= (int)sizeof(s_render_buf)) break;
            pos += n;
        }
    }
    s_render_buf[pos] = '\0';
    lv_label_set_text(s_out_lbl, s_render_buf);

    if (s_hint_lbl) {
        char h[96];
        if (s_hist.count > 0) {
            snprintf(h, sizeof(h), "A:%s >:%s <:%s B:%s",
                     lang_get(STR_COMMON_CONFIRM), lang_get(STR_SHELL_INPUT),
                     lang_get(STR_SHELL_HISTORY), lang_get(STR_COMMON_BACK));
        } else {
            snprintf(h, sizeof(h), "<:%s B:%s",
                     lang_get(STR_SHELL_HISTORY), lang_get(STR_COMMON_BACK));
        }
        lv_label_set_text(s_hint_lbl, h);
    }
}

/* 根据当前视图渲染 */
static void shell_render_current(void)
{
    if (s_view == SHELL_VIEW_HIST) shell_render_history();
    else shell_render();
}

/* ========== 命令实现 ========== */
static void cmd_help(void)
{
    shell_append("%s", lang_get(STR_SHELL_AVAIL));
    shell_append(" help ls cat echo clear");
    shell_append(" sysinfo mem bat sd");
    shell_append(" brightness ver reboot");
    shell_append(" py <%s>", lang_get(STR_SHELL_CODE));
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
    shell_append("BAT: %u%% %umV", (unsigned)app_sys_battery_percent(),
                 (unsigned)app_sys_battery_mv());
    shell_append("SD: %s", app_sys_sdcard_ready() ? "ready" : "not ready");
    shell_append("XiaoMiao %s", XIAOMIAO_VERSION);
}

static void cmd_mem(void)
{
    shell_append("DRAM free: %luK", (unsigned long)(app_sys_heap_internal_free() / 1024));
    shell_append("PSRAM free: %luK", (unsigned long)(app_sys_psram_free() / 1024));
}

static void cmd_bat(void)
{
    shell_append("Battery: %u%% (%umV)", (unsigned)app_sys_battery_percent(),
                 (unsigned)app_sys_battery_mv());
}

static void cmd_sd(void)
{
    shell_append("SD card: %s", app_sys_sdcard_ready() ? "mounted" : "not mounted");
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
    /* 门面层统一更新 ui_state 并驱动背光；save_all 持久化到 NVS，避免重启丢失 */
    app_sys_cfg_set_int(APP_CFG_BRIGHTNESS, val);
    app_sys_cfg_save_all();
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
    shell_hist_add(cmdline);          /* 记录历史并持久化（含历史重放） */
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
    else if (strcmp(cmd, "mem") == 0) cmd_mem();
    else if (strcmp(cmd, "bat") == 0) cmd_bat();
    else if (strcmp(cmd, "sd") == 0) cmd_sd();
    else if (strcmp(cmd, "brightness") == 0) cmd_brightness(arg);
    else if (strcmp(cmd, "ver") == 0) shell_append("XiaoMiao %s (%s)", XIAOMIAO_VERSION, XIAOMIAO_BUILD);
    else if (strcmp(cmd, "py") == 0) cmd_py(arg);
    else if (strcmp(cmd, "reboot") == 0) { shell_append("rebooting..."); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }
    else if (cmd[0] == '\0') { /* 空命令 */ }
    else shell_append("%s: %s (%s)", lang_get(STR_SHELL_UNKNOWN), cmd, lang_get(STR_SHELL_HELP));
}

/* ========== 键盘确认/取消回调 ========== */
static void shell_kb_confirm(const char *text, void *user_data)
{
    (void)user_data;
    ui_keyboard_hide();
    /* 无论从输出视图还是历史视图（预填编辑）提交，都回到输出视图展示结果 */
    s_view = SHELL_VIEW_OUTPUT;
    shell_execute(text);
}

static void shell_kb_cancel(void *user_data)
{
    (void)user_data;
    ui_keyboard_hide();
}

/* 统一入口：可传入历史命令作为预填值。
 * 键盘内部无光标移动能力，故编辑方式为「回填后在末尾追加 / 退格」。 */
static void shell_open_keyboard(const char *prefill)
{
    kb_config_t cfg = {
        .title = lang_get(STR_SHELL_TITLE),
        .placeholder = ">",
        .max_length = SHELL_HIST_LEN,       /* 允许 63 字符，完整容纳历史命令 */
        .password_mode = false,
        .on_confirm = shell_kb_confirm,
        .on_cancel = shell_kb_cancel,
        .user_data = NULL,
    };
    ui_keyboard_show(&cfg);
    /* 必须在 show 之后调用：此时输入框对象已创建，set_text 会刷新显示 */
    if (prefill && prefill[0]) ui_keyboard_set_text(prefill);
}

/* ========== 页面生命周期 ========== */
static void shell_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Shell app init");
    shell_hist_load();                  /* 启动时从 NVS 恢复历史 */
    s_view = SHELL_VIEW_OUTPUT;
    s_hist_sel = 0;

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
            lv_obj_set_size(container, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 16);
            lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
            /* 给 label 设置父对象为 container，但 label 自身承载文本 */
            lv_obj_set_parent(s_out_lbl, container);
            lv_obj_set_style_text_color(s_out_lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_style_text_font(s_out_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_bg_color(s_out_lbl, lv_color_hex(colors->bg), 0);
            lv_obj_set_style_bg_opa(s_out_lbl, LV_OPA_COVER, 0);
            lv_obj_set_pos(s_out_lbl, 3, 1);
            lv_obj_set_size(s_out_lbl, LCD_H_RES - 6, LCD_V_RES - ui_content_y() - DOCK_H - 16);
            lv_label_set_long_mode(s_out_lbl, LV_LABEL_LONG_WRAP);
            lv_label_set_text(s_out_lbl, "");
        }
    }

    /* 底部提示 */
    s_hint_lbl = lv_label_create(scr);
    if (s_hint_lbl) {
        lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(s_hint_lbl, lv_font_cn_get(12), 0);
        lv_obj_set_pos(s_hint_lbl, 2, LCD_V_RES - DOCK_H - 15);
        {
            char h[96];
            snprintf(h, sizeof(h), "A:%s  <:%s  B:%s", lang_get(STR_SHELL_INPUT),
                     lang_get(STR_SHELL_HISTORY), lang_get(STR_COMMON_BACK));
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
    shell_render_current();
}

static void shell_destroy(void)
{
    ESP_LOGI(TAG, "Shell app destroy");
    if (ui_keyboard_is_visible()) ui_keyboard_hide();
    s_out_lbl = NULL;
    s_hint_lbl = NULL;
    s_count = 0;
    s_scroll = 0;
    s_view = SHELL_VIEW_OUTPUT;   /* 历史数据保留在 s_hist，已持久化，不清空 */
    s_hist_sel = 0;
}

static bool shell_on_key(int key)
{
    /* 键盘可见时，按键交给键盘处理（含其字符网格导航） */
    if (ui_keyboard_is_visible()) {
        return ui_keyboard_on_key(key);
    }

    /* ========== 历史视图：UP/DOWN 切换命令，而非滚动输出 ========== */
    if (s_view == SHELL_VIEW_HIST) {
        switch (key) {
        case KEY_UP:
            if (s_hist.count > 0 && s_hist_sel > 0) {
                s_hist_sel--;
                shell_render_history();
            }
            return true;
        case KEY_DOWN:
            if (s_hist.count > 0 && s_hist_sel < s_hist.count - 1) {
                s_hist_sel++;
                shell_render_history();
            }
            return true;
        case KEY_A: {
            /* 直接重新执行选中命令。先拷贝到局部变量，避免
             * shell_execute -> shell_hist_add 写入环形缓冲时与源指针重叠。 */
            if (s_hist.count > 0) {
                char cmd[SHELL_HIST_LEN];
                char *item = shell_hist_item(s_hist_sel);
                if (item) {
                    strncpy(cmd, item, sizeof(cmd) - 1);
                    cmd[sizeof(cmd) - 1] = '\0';
                    s_view = SHELL_VIEW_OUTPUT;   /* 回输出视图展示执行结果 */
                    shell_execute(cmd);
                }
            }
            return true;
        }
        case KEY_RIGHT: {
            /* 回填到输入框：键盘预填选中命令，可追加/退格后确认执行 */
            if (s_hist.count > 0) {
                char *item = shell_hist_item(s_hist_sel);
                if (item) shell_open_keyboard(item);
            }
            return true;
        }
        case KEY_LEFT:
        case KEY_B:
            s_view = SHELL_VIEW_OUTPUT;
            shell_render_current();
            return true;
        default:
            return false;
        }
    }

    /* ========== 输出视图 ========== */
    switch (key) {
    case KEY_A:
        shell_open_keyboard(NULL);            /* 空输入框，输入新命令 */
        return true;
    case KEY_LEFT:
    case KEY_RIGHT:
        s_view = SHELL_VIEW_HIST;             /* 切到历史视图 */
        s_hist_sel = (s_hist.count > 0) ? (s_hist.count - 1) : 0;
        shell_render_history();
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
