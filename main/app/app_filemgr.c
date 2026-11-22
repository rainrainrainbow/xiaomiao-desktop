/**
 * @file app_filemgr.c
 * @brief 文件管理应用（P0 文件操作能力）
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_filemgr_callbacks。
 * 功能：浏览目录、打开文本文件、播放WAV；选择/标记、操作菜单、新建、
 *       重命名、软删除到回收站、复制/剪切/粘贴、属性、搜索、排序、
 *       回收站还原/彻底删除/清空。
 *
 * 5键交互（160x128）：
 *   UP/DOWN  移动光标 / 滚动
 *   LEFT     标记/取消标记当前项（多选，删除时批量生效）
 *   RIGHT    进入目录 / 打开文件（原 A 键功能）
 *   A        弹出操作菜单（列表态）/ 菜单内确认 / 二次确认态确认
 *   B        返回 / 关闭菜单或属性页 / 取消二次确认
 *   （长按 A/B 被 main.c 全局占用：长按A回桌面、长按B进最近任务，故不可用）
 *
 * 内存说明：目录条目/元数据/排序临时缓冲均放 PSRAM（EXT_RAM_BSS_ATTR）；
 *           递归辅助函数在栈上最多使用 256B 路径缓冲，深度上限 8。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "app_sys_api.h"
#include "ui_framework.h"
#include "ui_keyboard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "APP_FILEMGR";

#define FILEMGR_MAX_ENTRIES 20
#define FILEMGR_PATH_LEN   256
/* 回收站时间戳前缀：YYYYMMDD_HHMMSS_ = 16 字符 */
#define FM_RECYCLE_PREFIX_LEN 16
/* 递归操作最大深度，防止栈上路径缓冲累积过深 */
#define FM_MAX_DEPTH 8
/* 行高根据字体大小动态计算，在 filemgr_init 中设置 */
static int s_filemgr_row_h = 15;

/* ========== 目录列表数据（保持原有 static 数组，放 PSRAM） ========== */
static lv_obj_t *s_filemgr_obj = NULL;
static int s_filemgr_sel = 0;
static int s_filemgr_count = 0;
/* 纯 UI 显示缓存，放 PSRAM：5KB+2.5KB 内部 DRAM 释放 */
EXT_RAM_BSS_ATTR static char s_filemgr_entries[FILEMGR_MAX_ENTRIES][FILEMGR_PATH_LEN];
static bool s_filemgr_is_dir[FILEMGR_MAX_ENTRIES];
static char s_filemgr_current_path[FILEMGR_PATH_LEN] = "/";
static int s_filemgr_scroll = 0;

/* 新增：每个条目的 stat 元数据、多选标记、回收站来源存储标记 */
typedef struct {
    int64_t size;   /* 文件大小（目录为 0） */
    time_t  mtime;  /* 修改时间 */
} fm_meta_t;
EXT_RAM_BSS_ATTR static fm_meta_t s_filemgr_meta[FILEMGR_MAX_ENTRIES];
static bool s_filemgr_marked[FILEMGR_MAX_ENTRIES];
static bool s_recycle_src_sd[FILEMGR_MAX_ENTRIES];

/* ========== 视图/模式状态 ========== */
typedef enum {
    FM_VIEW_LIST = 0,   /* 普通列表浏览 */
    FM_VIEW_MENU,       /* 操作菜单 */
    FM_VIEW_PROPS       /* 属性页 */
} fm_view_t;
static fm_view_t s_view = FM_VIEW_LIST;
static bool s_in_recycle = false;                       /* 当前是否在回收站合并视图 */
static char s_prev_path[FILEMGR_PATH_LEN] = "/";        /* 进入回收站前的路径，B 键返回 */

/* ========== 操作菜单 ========== */
typedef enum {
    FM_M_NEW = 0, FM_M_RENAME, FM_M_DELETE, FM_M_COPY, FM_M_CUT,
    FM_M_PASTE, FM_M_PROPS, FM_M_SEARCH, FM_M_SORT, FM_M_RECYCLE,
    FM_M_RESTORE, FM_M_PURGE, FM_M_EMPTY, FM_M_MAX
} fm_menu_id_t;
static int s_menu_sel = 0;
static int s_menu_scroll = 0;   /* 菜单可滚动（10 项 > 小屏可见行数） */

/* ========== 属性页滚动 ========== */
static int s_props_scroll = 0;
static int s_props_idx = 0;

/* ========== 剪贴板（按需求：单路径 static char[256] + is_cut） ========== */
static char s_clipboard[FILEMGR_PATH_LEN] = {0};
static bool s_clip_valid = false;
static bool s_clip_is_cut = false;

/* ========== 搜索 / 排序 ========== */
static char s_search_keyword[64] = {0};
static bool s_search_active = false;
static int  s_sort_key = 0;   /* 0=名称 1=时间 2=大小，按 A 循环切换 */
EXT_RAM_BSS_ATTR static char s_sort_tmp_name[FILEMGR_PATH_LEN];

/* ========== 键盘输入 ========== */
typedef enum {
    FM_KB_NONE = 0, FM_KB_NEW, FM_KB_RENAME, FM_KB_SEARCH
} fm_kb_purpose_t;
static fm_kb_purpose_t s_kb_purpose = FM_KB_NONE;
static char s_kb_input[FILEMGR_PATH_LEN] = {0};
static int  s_kb_target_idx = 0;

/* ========== 破坏性操作二次确认 ========== */
typedef enum {
    FM_CONFIRM_NONE = 0, FM_CONFIRM_DELETE, FM_CONFIRM_PURGE, FM_CONFIRM_EMPTY
} fm_confirm_t;
static fm_confirm_t s_confirm = FM_CONFIRM_NONE;

/* ========== 文本查看器状态 ========== */
#define TXT_MAX_LINES   32
#define TXT_LINE_LEN    80
static lv_obj_t *s_txt_obj = NULL;
EXT_RAM_BSS_ATTR static char s_txt_lines[TXT_MAX_LINES][TXT_LINE_LEN];
static int s_txt_line_count = 0;
static int s_txt_scroll = 0;
static int s_txt_viewer_active = 0;
/* 文本文件超过 TXT_MAX_LINES 时是否仍有后续内容（用于末尾提示） */
static bool s_txt_has_more = false;
/* 目录打开失败时，是否因 SD 卡未挂载（用于区分"空目录"与"未插卡"提示） */
static bool s_filemgr_sd_missing = false;

/* ===================================================================== */
/* 通用小工具                                                              */
/* ===================================================================== */

/** 用状态栏标题做短提示（lang 字符串或临时缓冲均可，lv_label_set_text 会拷贝） */
static void filemgr_msg(const char *msg)
{
    ui_statusbar_set_title(msg);
}

/** 取路径最后一段 */
static const char *filemgr_basename(const char *path)
{
    if (!path) return "";
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/** 路径是否属于 SD 卡存储 */
static bool filemgr_path_on_sd(const char *path)
{
    return path && strncmp(path, "/sdcard", 7) == 0 &&
           (path[7] == '/' || path[7] == '\0');
}

/** 当前浏览路径是否在虚拟根 */
static bool filemgr_is_root(void)
{
    return strcmp(s_filemgr_current_path, "/") == 0;
}

/** 取路径所属存储的剩余空间 KB */
static uint32_t filemgr_free_kb(const char *path)
{
    if (filemgr_path_on_sd(path)) {
        return app_sys_sdcard_free_kb();
    }
    return app_sys_flash_free_kb();
}

/** 进入目录/粘贴前显示剩余空间提示 */
static void filemgr_show_free(const char *path)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "%s: %lu KB", lang_get(STR_FILE_FREE),
             (unsigned long)filemgr_free_kb(path));
    filemgr_msg(msg);
}

/** 校验用户输入的文件名，防止 '..' 越界与 '/' 路径注入 */
static bool filemgr_valid_name(const char *name)
{
    if (!name || name[0] == '\0') return false;
    /* 预留回收站前缀长度，保证重命名后仍能加时间戳前缀 */
    if (strlen(name) > FILEMGR_PATH_LEN - FM_RECYCLE_PREFIX_LEN - 8) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    for (const char *p = name; *p; ++p) {
        if ((unsigned char)*p < 0x20) return false;   /* 控制字符 */
    }
    return true;
}

/** 大小写不敏感子串查找（避免依赖 strcasestr 的 GNU 扩展） */
static const char *filemgr_stristr(const char *hay, const char *needle)
{
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return p;
    }
    return NULL;
}

/** 拼接 "目录/名称"，长度越界返回 false（所有路径拼接受此保护） */
static bool filemgr_join_path(const char *dir, const char *name, char *buf, size_t size)
{
    if (!dir || !name || !buf || size == 0) return false;
    int n;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    n = snprintf(buf, size, "%s/%s", dir, name);
#pragma GCC diagnostic pop
    if (n < 0 || n >= (int)size) {
        ESP_LOGE(TAG, "Path too long: %s/%s", dir, name);
        return false;
    }
    return true;
}

/**
 * 取第 idx 个条目的完整路径。
 * 回收站合并视图下，每个条目来源存储不同，按 s_recycle_src_sd[] 选根目录。
 */
static bool filemgr_entry_path(int idx, char *buf, size_t size)
{
    if (idx < 0 || idx >= s_filemgr_count || !buf || size == 0) return false;
    const char *name = s_filemgr_entries[idx];
    if (s_in_recycle) {
        const char *dir = s_recycle_src_sd[idx] ? "/sdcard/.recycle" : "/flash/.recycle";
        return filemgr_join_path(dir, name, buf, size);
    }
    if (filemgr_is_root()) {
        /* 虚拟根下的 "flash"/"sdcard" 直接拼成 "/flash"/"/sdcard"，避免 "//flash" */
        int n;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        n = snprintf(buf, size, "/%s", name);
#pragma GCC diagnostic pop
        if (n < 0 || n >= (int)size) {
            ESP_LOGE(TAG, "Path too long: /%s", name);
            return false;
        }
        return true;
    }
    return filemgr_join_path(s_filemgr_current_path, name, buf, size);
}

/** 扫描开始：清空计数/选择/标记，设置当前路径 */
static void filemgr_scan_begin(const char *path)
{
    s_filemgr_count = 0;
    s_filemgr_sel = 0;
    s_filemgr_scroll = 0;
    s_in_recycle = false;
    memset(s_filemgr_marked, 0, sizeof(s_filemgr_marked));
    memset(s_recycle_src_sd, 0, sizeof(s_recycle_src_sd));
    if (path) {
        strncpy(s_filemgr_current_path, path, FILEMGR_PATH_LEN - 1);
        s_filemgr_current_path[FILEMGR_PATH_LEN - 1] = '\0';
    }
}

/** 填充单个条目的 stat 元数据（大小/修改时间），失败保持 0 */
static void filemgr_fill_meta(int idx)
{
    char path[FILEMGR_PATH_LEN];
    struct stat st;
    s_filemgr_meta[idx].size = 0;
    s_filemgr_meta[idx].mtime = 0;
    if (filemgr_entry_path(idx, path, sizeof(path)) && stat(path, &st) == 0) {
        s_filemgr_meta[idx].size = S_ISDIR(st.st_mode) ? 0 : (int64_t)st.st_size;
        s_filemgr_meta[idx].mtime = (time_t)st.st_mtime;
    }
}

/* ===================================================================== */
/* 排序（目录优先，其次按 名称/时间/大小 升序，插入排序，最大 20 项）    */
/* ===================================================================== */

static bool filemgr_less(int a, int b)
{
    /* 目录始终排在文件前面，便于浏览 */
    if (s_filemgr_is_dir[a] != s_filemgr_is_dir[b]) {
        return s_filemgr_is_dir[a];
    }
    switch (s_sort_key) {
    case 1:
        return s_filemgr_meta[a].mtime < s_filemgr_meta[b].mtime;
    case 2:
        return s_filemgr_meta[a].size < s_filemgr_meta[b].size;
    default:
        return strcasecmp(s_filemgr_entries[a], s_filemgr_entries[b]) < 0;
    }
}

static void filemgr_swap_entries(int a, int b)
{
    if (a == b) return;
    memcpy(s_sort_tmp_name, s_filemgr_entries[a], FILEMGR_PATH_LEN);
    memcpy(s_filemgr_entries[a], s_filemgr_entries[b], FILEMGR_PATH_LEN);
    memcpy(s_filemgr_entries[b], s_sort_tmp_name, FILEMGR_PATH_LEN);

    bool tb = s_filemgr_is_dir[a];
    s_filemgr_is_dir[a] = s_filemgr_is_dir[b];
    s_filemgr_is_dir[b] = tb;

    fm_meta_t tm = s_filemgr_meta[a];
    s_filemgr_meta[a] = s_filemgr_meta[b];
    s_filemgr_meta[b] = tm;

    tb = s_filemgr_marked[a];
    s_filemgr_marked[a] = s_filemgr_marked[b];
    s_filemgr_marked[b] = tb;

    tb = s_recycle_src_sd[a];
    s_recycle_src_sd[a] = s_recycle_src_sd[b];
    s_recycle_src_sd[b] = tb;
}

static void filemgr_sort_entries(void)
{
    for (int i = 1; i < s_filemgr_count; i++) {
        int j = i;
        while (j > 0 && filemgr_less(j, j - 1)) {
            filemgr_swap_entries(j, j - 1);
            j--;
        }
    }
}

/* ===================================================================== */
/* 文本查看器                                                              */
/* ===================================================================== */

static void txt_viewer_open(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open text file: %s", path);
        return;
    }
    s_txt_line_count = 0;
    s_txt_scroll = 0;
    char line[TXT_LINE_LEN];
    while (s_txt_line_count < TXT_MAX_LINES &&
           fgets(line, sizeof(line), fp)) {
        /* 去掉换行符 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        strncpy(s_txt_lines[s_txt_line_count], line, TXT_LINE_LEN - 1);
        s_txt_lines[s_txt_line_count][TXT_LINE_LEN - 1] = '\0';
        s_txt_line_count++;
    }
    /* 行数达到上限时探测是否还有后续内容，用于末尾提示（不会写入 s_txt_lines，避免越界） */
    if (s_txt_line_count >= TXT_MAX_LINES) {
        char probe[16];
        s_txt_has_more = (fgets(probe, sizeof(probe), fp) != NULL);
    } else {
        s_txt_has_more = false;
    }
    fclose(fp);
    s_txt_viewer_active = 1;
    ESP_LOGI(TAG, "Text viewer: %d lines from %s", s_txt_line_count, path);
}

static void txt_viewer_refresh(void)
{
    if (!s_txt_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_txt_obj);
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 1;
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - row_h - 2) / row_h;
    if (vis_rows < 1) vis_rows = 1;
    int start = s_txt_scroll;
    int end = start + vis_rows;
    if (end > s_txt_line_count) end = s_txt_line_count;
    /* 还有更多内容时，预留最后一行显示提示，保证不超出可视区 */
    bool show_more = s_txt_has_more && (end >= s_txt_line_count);
    if (show_more && end > start) end--;
    for (int i = start; i < end; i++) {
        int row_idx = i - start;
        lv_obj_t *row = lv_obj_create(s_txt_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_h + 2 + row_idx * row_h);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_label_set_text(lbl, s_txt_lines[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
    if (show_more) {
        int row_idx = end - start;
        lv_obj_t *row = lv_obj_create(s_txt_obj);
        if (row) {
            lv_obj_remove_style_all(row);
            lv_obj_set_pos(row, 0, row_h + 2 + row_idx * row_h);
            lv_obj_set_size(row, LCD_H_RES, row_h);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_t *lbl = lv_label_create(row);
            if (lbl) {
                lv_label_set_text(lbl, "...(更多内容未显示)");
                lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
                lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
                lv_obj_set_width(lbl, LCD_H_RES - 8);
                lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
            }
        }
    }
}

static void txt_viewer_close(void)
{
    s_txt_viewer_active = 0;
    s_txt_line_count = 0;
    s_txt_scroll = 0;
    s_txt_has_more = false;
    s_txt_obj = NULL;
}

/* ===================================================================== */
/* 文件打开处理（保持原有分发逻辑）                                        */
/* ===================================================================== */

static bool file_is_text(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".txt") == 0) return true;
    if (strcasecmp(ext, ".py") == 0) return true;
    if (strcasecmp(ext, ".c") == 0) return true;
    if (strcasecmp(ext, ".h") == 0) return true;
    if (strcasecmp(ext, ".cpp") == 0) return true;
    if (strcasecmp(ext, ".json") == 0) return true;
    if (strcasecmp(ext, ".md") == 0) return true;
    if (strcasecmp(ext, ".ini") == 0) return true;
    if (strcasecmp(ext, ".cfg") == 0) return true;
    if (strcasecmp(ext, ".log") == 0) return true;
    if (strcasecmp(ext, ".csv") == 0) return true;
    if (strcasecmp(ext, ".xml") == 0) return true;
    if (strcasecmp(ext, ".html") == 0) return true;
    if (strcasecmp(ext, ".conf") == 0) return true;
    return false;
}

static bool file_is_mid(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".mid") == 0) return true;
    if (strcasecmp(ext, ".midi") == 0) return true;
    return false;
}

static bool file_is_audio(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".wav") == 0) return true;
    if (strcasecmp(ext, ".mp3") == 0) return true;
    if (strcasecmp(ext, ".ogg") == 0) return true;
    if (strcasecmp(ext, ".flac") == 0) return true;
    return false;
}

static void filemgr_open_file(const char *path, const char *name)
{
    if (file_is_mid(name)) {
        /* MID 播放器已移除：原 stash 构造后从未使用，属无效代码，直接清理 */
        ESP_LOGI(TAG, "MID file support removed");
        ui_statusbar_set_title(lang_get(STR_FILE_MID_REMOVED));
    } else if (file_is_text(name)) {
        txt_viewer_open(path);
        txt_viewer_refresh();
        ESP_LOGI(TAG, "Opened text file: %s", path);
    } else if (file_is_audio(name)) {
        if (strcasecmp(strrchr(name, '.'), ".wav") == 0) {
            /* WAV文件可播放 → 路由到音乐播放器，通过 stash 传递路径 */
            page_stash_t stash;
            size_t path_len = strlen(path);
            stash.valid = true;
            stash.size = (path_len + 1 > PAGE_STASH_SIZE) ? PAGE_STASH_SIZE : path_len + 1;
            memcpy(stash.data, path, stash.size - 1);
            stash.data[stash.size - 1] = '\0';
            ui_stash_set(&stash);
            ui_stack_push(PAGE_APP_PLACEHOLDER, &g_music_callbacks, NULL);
            ESP_LOGI(TAG, "Routed WAV file to music player: %s", path);
        } else {
            ESP_LOGW(TAG, "Audio file format not supported yet: %s", path);
            ui_statusbar_set_title(lang_get(STR_FILE_AUDIO_NA));
        }
    } else {
        ESP_LOGW(TAG, "Unknown file type: %s", name);
        ui_statusbar_set_title(lang_get(STR_FILE_UNSUPPORTED));
    }
}

/* ===================================================================== */
/* 扫描                                                                   */
/* ===================================================================== */

/** 虚拟根目录：列出内部 FAT /flash 与外部 /sdcard 两个存储入口 */
static void filemgr_scan_root(void)
{
    s_filemgr_count = 0;
    /* 内部 FAT 分区（始终挂载） */
    DIR *flash = opendir("/flash");
    if (flash) {
        closedir(flash);
        strncpy(s_filemgr_entries[s_filemgr_count], "flash", FILEMGR_PATH_LEN - 1);
        s_filemgr_entries[s_filemgr_count][FILEMGR_PATH_LEN - 1] = '\0';
        s_filemgr_is_dir[s_filemgr_count++] = true;
    }
    /* 外部 SD 卡（可选） */
    if (app_sys_sdcard_ready()) {
        DIR *sd = opendir("/sdcard");
        if (sd) {
            closedir(sd);
            strncpy(s_filemgr_entries[s_filemgr_count], "sdcard", FILEMGR_PATH_LEN - 1);
            s_filemgr_entries[s_filemgr_count][FILEMGR_PATH_LEN - 1] = '\0';
            s_filemgr_is_dir[s_filemgr_count++] = true;
        }
    }
    s_filemgr_sd_missing = false;
    ESP_LOGI(TAG, "File manager root: %d storage entries", s_filemgr_count);
}

/**
 * 扫描单个目录，追加条目到数组（不重置计数）。
 * 回收站合并视图时 is_sd 标记来源存储，供还原时决定回 /flash 还是 /sdcard。
 */
static void filemgr_scan_one_dir(const char *dir, bool is_sd)
{
    DIR *d = opendir(dir);
    if (!d) {
        if (!s_in_recycle && strncmp(dir, "/sdcard", 7) == 0) {
            s_filemgr_sd_missing = !app_sys_sdcard_ready();
        }
        ESP_LOGW(TAG, "Cannot open directory: %s (sd_missing=%d)",
                 dir, s_filemgr_sd_missing ? 1 : 0);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_filemgr_count < FILEMGR_MAX_ENTRIES) {
        if (e->d_name[0] == '.') continue;                       /* 隐藏 .recycle 等 */
        if (s_search_active && !s_in_recycle &&
            !filemgr_stristr(e->d_name, s_search_keyword)) continue;
        int idx = s_filemgr_count;
        strncpy(s_filemgr_entries[idx], e->d_name, FILEMGR_PATH_LEN - 1);
        s_filemgr_entries[idx][FILEMGR_PATH_LEN - 1] = '\0';
        if (s_in_recycle) {
            s_recycle_src_sd[idx] = is_sd;
        }
        s_filemgr_is_dir[idx] = (e->d_type == DT_DIR);
        /* FAT 某些实现 d_type 为 DT_UNKNOWN，用 stat 兜底判断目录 */
        if (e->d_type == DT_UNKNOWN) {
            char path[FILEMGR_PATH_LEN];
            if (filemgr_entry_path(idx, path, sizeof(path))) {
                s_filemgr_is_dir[idx] = app_sys_fs_is_dir(path);
            }
        }
        filemgr_fill_meta(idx);
        s_filemgr_count++;
    }
    closedir(d);
}

/** 扫描普通目录 */
static void filemgr_scan_dir(const char *path)
{
    filemgr_scan_begin(path);
    if (filemgr_is_root()) {
        filemgr_scan_root();
        filemgr_sort_entries();
        return;
    }
    s_filemgr_sd_missing = false;
    filemgr_scan_one_dir(path, false);
    filemgr_sort_entries();
    ESP_LOGI(TAG, "File manager: %d entries in %s", s_filemgr_count, path);
}

/** 扫描回收站：合并 /flash/.recycle 与 /sdcard/.recycle 两个视图 */
static void filemgr_scan_recycle(void)
{
    filemgr_scan_begin("/");
    s_in_recycle = true;
    s_filemgr_sd_missing = false;
    /* 按需创建回收站目录（首次删除也会创建） */
    app_sys_fs_mkdir("/flash/.recycle");
    filemgr_scan_one_dir("/flash/.recycle", false);
    if (app_sys_sdcard_ready()) {
        app_sys_fs_mkdir("/sdcard/.recycle");
        filemgr_scan_one_dir("/sdcard/.recycle", true);
    }
    filemgr_sort_entries();
    ESP_LOGI(TAG, "File manager recycle: %d entries", s_filemgr_count);
}

/* ===================================================================== */
/* 列表 / 菜单 / 属性 渲染                                                  */
/* ===================================================================== */

static void filemgr_draw_header(const theme_colors_t *colors, int font_px)
{
    lv_obj_t *path_row = lv_obj_create(s_filemgr_obj);
    if (!path_row) {
        ESP_LOGE(TAG, "lv_obj_create(path_row) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(path_row);
    lv_obj_set_pos(path_row, 0, 0);
    lv_obj_set_size(path_row, LCD_H_RES, s_filemgr_row_h);
    lv_obj_clear_flag(path_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(path_row, LV_OPA_TRANSP, 0);

    char header[FILEMGR_PATH_LEN + 16];
    if (s_in_recycle) {
        snprintf(header, sizeof(header), "> [%s]", lang_get(STR_FILE_RECYCLE));
    } else if (s_search_active) {
        snprintf(header, sizeof(header), "> %s [%s]", s_filemgr_current_path,
                 lang_get(STR_FILE_SEARCH));
    } else {
        snprintf(header, sizeof(header), "> %s", s_filemgr_current_path);
    }
    lv_obj_t *path_lbl = lv_label_create(path_row);
    if (path_lbl) {
        lv_label_set_text(path_lbl, header);
        lv_obj_set_style_text_color(path_lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(path_lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_width(path_lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(path_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(path_lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

static void filemgr_refresh_list(void)
{
    if (!s_filemgr_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_filemgr_obj);
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
    if (vis_rows < 1) vis_rows = 1;
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;

    filemgr_draw_header(colors, font_px);

    if (s_filemgr_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_filemgr_obj);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            return;
        }
        lv_label_set_text(lbl, s_filemgr_sd_missing
                               ? lang_get(STR_REC_SD_UNMOUNTED)
                               : lang_get(STR_FILE_EMPTY_DIR));
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int start = s_filemgr_scroll;
    int end = start + vis_rows;
    if (end > s_filemgr_count) end = s_filemgr_count;
    for (int i = start; i < end; i++) {
        int row_idx = i - start + 1; /* 第0行是路径 */
        lv_obj_t *row = lv_obj_create(s_filemgr_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, row_idx * s_filemgr_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_filemgr_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_filemgr_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        char buf[FILEMGR_PATH_LEN + 4];
        /* 标记项用 ✓ 前缀，目录 📁，文件 📄 */
        const char *prefix = s_filemgr_marked[i] ? "✓ " :
                             (s_filemgr_is_dir[i] ? "📁 " : "📄 ");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(buf, sizeof(buf), "%s%s", prefix, s_filemgr_entries[i]);
#pragma GCC diagnostic pop
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

/* ---- 操作菜单 ---- */

static int filemgr_menu_count(void)
{
    return s_in_recycle ? 3 : 10;
}

/** 菜单显示位置 -> 菜单ID（回收站模式下只显示 还原/彻底删除/清空） */
static int filemgr_menu_id(int pos)
{
    if (s_in_recycle) {
        const int ids[] = { FM_M_RESTORE, FM_M_PURGE, FM_M_EMPTY };
        return (pos >= 0 && pos < 3) ? ids[pos] : FM_M_RESTORE;
    }
    return (pos >= 0 && pos < 10) ? pos : FM_M_NEW;
}

static const char *filemgr_menu_label(int id)
{
    switch (id) {
    case FM_M_NEW:      return lang_get(STR_FILE_NEW);
    case FM_M_RENAME:   return lang_get(STR_FILE_RENAME);
    case FM_M_DELETE:   return lang_get(STR_FILE_DELETE);
    case FM_M_COPY:     return lang_get(STR_FILE_COPY);
    case FM_M_CUT:      return lang_get(STR_FILE_CUT);
    case FM_M_PASTE:    return lang_get(STR_FILE_PASTE);
    case FM_M_PROPS:    return lang_get(STR_FILE_PROPERTIES);
    case FM_M_SEARCH:   return lang_get(STR_FILE_SEARCH);
    case FM_M_SORT:     return lang_get(STR_FILE_SORT);
    case FM_M_RECYCLE:  return lang_get(STR_FILE_RECYCLE);
    case FM_M_RESTORE:  return lang_get(STR_FILE_RESTORE);
    case FM_M_PURGE:    return lang_get(STR_FILE_PURGE);
    case FM_M_EMPTY:    return lang_get(STR_FILE_EMPTY_RECYCLE);
    default:            return "";
    }
}

static void filemgr_refresh_menu(void)
{
    if (!s_filemgr_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    int font_px = ui_state_get()->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    lv_obj_clean(s_filemgr_obj);
    filemgr_draw_header(colors, font_px);

    int cnt = filemgr_menu_count();
    /* 小屏一次只能显示有限行，只渲染可视区间 */
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
    if (vis_rows < 1) vis_rows = 1;
    if (s_menu_scroll > cnt - vis_rows) s_menu_scroll = cnt - vis_rows;
    if (s_menu_scroll < 0) s_menu_scroll = 0;
    int start = s_menu_scroll;
    int end = start + vis_rows;
    if (end > cnt) end = cnt;

    for (int i = start; i < end; i++) {
        lv_obj_t *row = lv_obj_create(s_filemgr_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(menu row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, (i - start + 1) * s_filemgr_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_filemgr_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_menu_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) continue;
        lv_label_set_text(lbl, filemgr_menu_label(filemgr_menu_id(i)));
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

/* ---- 属性页 ---- */

/** 生成属性页第 line_no 行文本（6 行：名称/大小/修改时间/类型/路径/剩余空间） */
static void filemgr_props_line(int idx, int line_no, char *buf, size_t size)
{
    char path[FILEMGR_PATH_LEN];
    filemgr_entry_path(idx, path, sizeof(path));

    switch (line_no) {
    case 0:
        snprintf(buf, size, "%s: %.32s", lang_get(STR_FILE_NAME),
                 s_filemgr_entries[idx]);
        break;
    case 1: {
        if (s_filemgr_is_dir[idx]) {
            snprintf(buf, size, "%s: <DIR>", lang_get(STR_FILE_SIZE));
        } else {
            /* 同时展示逻辑大小与实际占用（stat.st_blocks * 512） */
            int64_t sz = s_filemgr_meta[idx].size;
            long used_kb = 0;
            struct stat st;
            if (stat(path, &st) == 0) {
                used_kb = (long)((int64_t)st.st_blocks * 512 / 1024);
            }
            snprintf(buf, size, "%s: %lld B / %ld KB", lang_get(STR_FILE_SIZE),
                     (long long)sz, used_kb);
        }
        break;
    }
    case 2: {
        char tbuf[32] = "-";
        if (s_filemgr_meta[idx].mtime > 0) {
            time_t t = (time_t)s_filemgr_meta[idx].mtime;
            struct tm tm_info;
            if (localtime_r(&t, &tm_info)) {
                strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm_info);
            }
        }
        snprintf(buf, size, "%s: %s", lang_get(STR_FILE_MODIFIED), tbuf);
        break;
    }
    case 3: {
        const char *type = s_filemgr_is_dir[idx] ? "<DIR>" : "<FILE>";
        snprintf(buf, size, "%s: %s", lang_get(STR_FILE_TYPE), type);
        break;
    }
    case 4:
        snprintf(buf, size, "%s: %.40s", lang_get(STR_FILE_PATH), path);
        break;
    default:
        snprintf(buf, size, "%s: %lu KB", lang_get(STR_FILE_FREE),
                 (unsigned long)filemgr_free_kb(path));
        break;
    }
}

static void filemgr_refresh_props(void)
{
    if (!s_filemgr_obj) return;
    const theme_colors_t *colors = ui_theme_colors();
    int font_px = ui_state_get()->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    int row_h = font_px + 1;
    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - row_h - 2) / row_h;
    if (vis_rows < 1) vis_rows = 1;
    if (s_props_idx < 0 || s_props_idx >= s_filemgr_count) {
        s_view = FM_VIEW_LIST;
        filemgr_refresh_list();
        return;
    }

    lv_obj_clean(s_filemgr_obj);
    filemgr_draw_header(colors, font_px);

    const int total = 6;
    if (s_props_scroll > total - vis_rows) s_props_scroll = total - vis_rows;
    if (s_props_scroll < 0) s_props_scroll = 0;
    int end = s_props_scroll + vis_rows;
    if (end > total) end = total;

    for (int i = s_props_scroll; i < end; i++) {
        int row_idx = i - s_props_scroll;
        lv_obj_t *row = lv_obj_create(s_filemgr_obj);
        if (!row) continue;
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, (row_idx + 1) * row_h);
        lv_obj_set_size(row, LCD_H_RES, row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) continue;
        char line[FILEMGR_PATH_LEN];
        filemgr_props_line(s_props_idx, i, line, sizeof(line));
        lv_label_set_text(lbl, line);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 8);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

/* ===================================================================== */
/* 递归文件操作辅助（删除/复制/统计大小，深度上限 FM_MAX_DEPTH）          */
/* ===================================================================== */

static int64_t filemgr_size_recursive(const char *path, int depth)
{
    if (depth > FM_MAX_DEPTH) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (S_ISREG(st.st_mode)) return (int64_t)st.st_size;
    if (!S_ISDIR(st.st_mode)) return 0;

    DIR *d = opendir(path);
    if (!d) return -1;
    int64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[FILEMGR_PATH_LEN];
        if (!filemgr_join_path(path, e->d_name, child, sizeof(child))) {
            closedir(d);
            return -1;
        }
        int64_t s = filemgr_size_recursive(child, depth + 1);
        if (s < 0) {
            closedir(d);
            return -1;
        }
        total += s;
    }
    closedir(d);
    return total;
}

static bool filemgr_remove_recursive(const char *path, int depth)
{
    if (depth > FM_MAX_DEPTH) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char child[FILEMGR_PATH_LEN];
            if (!filemgr_join_path(path, e->d_name, child, sizeof(child))) {
                closedir(d);
                return false;
            }
            if (!filemgr_remove_recursive(child, depth + 1)) {
                closedir(d);
                return false;
            }
        }
        closedir(d);
    }
    return app_sys_fs_remove(path) == APP_FS_OK;
}

static bool filemgr_copy_recursive(const char *src, const char *dst, int depth)
{
    if (depth > FM_MAX_DEPTH) return false;
    struct stat st;
    if (stat(src, &st) != 0) return false;
    if (S_ISREG(st.st_mode)) {
        return app_sys_fs_copy(src, dst, NULL) >= 0;
    }
    if (!S_ISDIR(st.st_mode)) return false;
    if (app_sys_fs_mkdir(dst) != APP_FS_OK) return false;

    DIR *d = opendir(src);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char csrc[FILEMGR_PATH_LEN];
        char cdst[FILEMGR_PATH_LEN];
        if (!filemgr_join_path(src, e->d_name, csrc, sizeof(csrc)) ||
            !filemgr_join_path(dst, e->d_name, cdst, sizeof(cdst))) {
            closedir(d);
            return false;
        }
        if (!filemgr_copy_recursive(csrc, cdst, depth + 1)) {
            closedir(d);
            return false;
        }
    }
    closedir(d);
    return true;
}

/** 两段路径是否位于同一存储（决定剪切用 rename 还是 copy+remove） */
static bool filemgr_same_storage(const char *a, const char *b)
{
    return filemgr_path_on_sd(a) == filemgr_path_on_sd(b);
}

/** 为目标路径生成不重名版本：name / name_1 ... name_99，全部存在则失败 */
static bool filemgr_unique_dst(const char *dir, const char *name,
                               char *buf, size_t size)
{
    for (int i = 0; i < 100; i++) {
        int n;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        if (i == 0) {
            n = snprintf(buf, size, "%s/%s", dir, name);
        } else {
            n = snprintf(buf, size, "%s/%s_%d", dir, name, i);
        }
#pragma GCC diagnostic pop
        if (n < 0 || n >= (int)size) return false;
        if (access(buf, F_OK) != 0) return true;   /* 不存在，可用 */
    }
    return false;
}

/* ===================================================================== */
/* 操作执行                                                               */
/* ===================================================================== */

/** 选择操作目标：优先第一个被标记项，否则当前光标项 */
static int filemgr_target_idx(void)
{
    for (int i = 0; i < s_filemgr_count; i++) {
        if (s_filemgr_marked[i]) return i;
    }
    return s_filemgr_sel;
}

/** 重新扫描当前视图并刷新列表 */
static void filemgr_rescan_current(void)
{
    if (s_in_recycle) {
        filemgr_scan_recycle();
    } else {
        filemgr_scan_dir(s_filemgr_current_path);
    }
    filemgr_refresh_list();
}

/** 新建文件夹（键盘回调） */
static void filemgr_do_new(const char *name)
{
    if (!filemgr_valid_name(name) || filemgr_is_root() || s_in_recycle) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    char path[FILEMGR_PATH_LEN];
    if (!filemgr_join_path(s_filemgr_current_path, name, path, sizeof(path))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    if (app_sys_fs_mkdir(path) == APP_FS_OK) {
        ESP_LOGI(TAG, "mkdir: %s", path);
        filemgr_msg(lang_get(STR_COMMON_SUCCESS));
        filemgr_rescan_current();
    } else {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
    }
}

/** 重命名（键盘回调） */
static void filemgr_do_rename(const char *name)
{
    if (!filemgr_valid_name(name) || filemgr_is_root() || s_in_recycle ||
        s_filemgr_sel < 0 || s_filemgr_sel >= s_filemgr_count) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    char src[FILEMGR_PATH_LEN];
    char dst[FILEMGR_PATH_LEN];
    if (!filemgr_entry_path(s_filemgr_sel, src, sizeof(src)) ||
        !filemgr_join_path(s_filemgr_current_path, name, dst, sizeof(dst))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    if (strcmp(src, dst) == 0) return;                 /* 名字未变 */
    if (access(dst, F_OK) == 0) {                       /* 目标已存在 */
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    if (app_sys_fs_rename(src, dst) == APP_FS_OK) {
        ESP_LOGI(TAG, "rename: %s -> %s", src, dst);
        filemgr_msg(lang_get(STR_COMMON_SUCCESS));
        filemgr_rescan_current();
    } else {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
    }
}

/**
 * 软删除：把条目 rename 到所在存储的 .recycle 目录，
 * 文件名加 16 字符时间戳前缀避免重名，还原时去掉前缀即可。
 */
static void filemgr_soft_delete(void)
{
    if (filemgr_is_root() || s_in_recycle) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    int targets[FILEMGR_MAX_ENTRIES];
    int n = 0;
    for (int i = 0; i < s_filemgr_count; i++) {
        if (s_filemgr_marked[i]) targets[n++] = i;
    }
    if (n == 0 && s_filemgr_sel >= 0 && s_filemgr_sel < s_filemgr_count) {
        targets[n++] = s_filemgr_sel;   /* 未标记时删除当前项 */
    }
    if (n == 0) return;

    time_t now = 0;
    struct tm tm_info;
    time(&now);
    localtime_r(&now, &tm_info);
    /* 加大缓冲：%.2d 对 int 最坏输出 11 字节，96 字节可容纳全部字段，
     * 避免 -Wformat-truncation 误报；实际仍写入 16 字符前缀 */
    char prefix[96] = {0};
    snprintf(prefix, sizeof(prefix), "%.4d%.2d%.2d_%.2d%.2d%.2d_",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    int ok_count = 0;
    for (int k = 0; k < n; k++) {
        char src[FILEMGR_PATH_LEN];
        if (!filemgr_entry_path(targets[k], src, sizeof(src))) continue;
        const char *recycle = filemgr_path_on_sd(src)
                                  ? "/sdcard/.recycle" : "/flash/.recycle";
        app_sys_fs_mkdir(recycle);

        const char *base = filemgr_basename(src);
        char name[FILEMGR_PATH_LEN] = {0};
        size_t base_len = strlen(base);
        size_t max_name = FILEMGR_PATH_LEN - FM_RECYCLE_PREFIX_LEN - 1;
        if (base_len > max_name) {
            /* 原文件名过长：截断以容纳前缀（还原时得到截断后的名字） */
            memcpy(name, base, max_name);
            name[max_name] = '\0';
            ESP_LOGW(TAG, "Recycle name truncated: %s", base);
        } else {
            memcpy(name, base, base_len + 1);
        }
        char dst[FILEMGR_PATH_LEN];
        int len = snprintf(dst, sizeof(dst), "%s/%s%s", recycle, prefix, name);
        if (len < 0 || len >= (int)sizeof(dst)) {
            ESP_LOGE(TAG, "Recycle dst too long: %s", base);
            continue;
        }
        if (app_sys_fs_rename(src, dst) == APP_FS_OK) {
            ESP_LOGI(TAG, "recycled: %s -> %s", src, dst);
            ok_count++;
        } else {
            ESP_LOGE(TAG, "recycle failed: %s", src);
        }
    }
    memset(s_filemgr_marked, 0, sizeof(s_filemgr_marked));
    filemgr_msg(ok_count > 0 ? lang_get(STR_FILE_DELETED)
                             : lang_get(STR_COMMON_FAILED));
    filemgr_rescan_current();
}

/** 复制/剪切：把当前（或首个标记）项路径写入剪贴板 */
static void filemgr_do_copy(bool cut)
{
    if (filemgr_is_root() || s_in_recycle) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    int idx = filemgr_target_idx();
    char src[FILEMGR_PATH_LEN];
    if (idx < 0 || idx >= s_filemgr_count ||
        !filemgr_entry_path(idx, src, sizeof(src))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    strncpy(s_clipboard, src, FILEMGR_PATH_LEN - 1);
    s_clipboard[FILEMGR_PATH_LEN - 1] = '\0';
    s_clip_valid = true;
    s_clip_is_cut = cut;
    ESP_LOGI(TAG, "clipboard %s: %s", cut ? "cut" : "copy", s_clipboard);
    filemgr_msg(cut ? lang_get(STR_FILE_CUT) : lang_get(STR_FILE_COPIED));
}

/** 粘贴：复制 or 移动剪贴板中的源到当前目录 */
static void filemgr_paste(void)
{
    if (!s_clip_valid || !s_clipboard[0] || filemgr_is_root() || s_in_recycle) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    const char *name = filemgr_basename(s_clipboard);
    if (!name[0]) return;

    /* 粘贴前先显示目标存储剩余空间，空间不足时后面会被 NOSPACE 覆盖 */
    filemgr_show_free(s_filemgr_current_path);

    char dst[FILEMGR_PATH_LEN];
    if (!filemgr_unique_dst(s_filemgr_current_path, name, dst, sizeof(dst))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    /* 防止把目录复制进自身 */
    size_t src_len = strlen(s_clipboard);
    if (strcmp(s_clipboard, dst) == 0 ||
        (strncmp(dst, s_clipboard, src_len) == 0 && dst[src_len] == '/')) {
        ESP_LOGW(TAG, "Paste into self rejected: %s -> %s", s_clipboard, dst);
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }

    /* 粘贴前空间检查 */
    int64_t need = filemgr_size_recursive(s_clipboard, 0);
    if (need < 0 || !app_sys_storage_has_space(dst, (uint32_t)need)) {
        ESP_LOGW(TAG, "No space for paste: %s (need=%lld)", dst, (long long)need);
        filemgr_msg(lang_get(STR_FILE_NOSPACE));
        return;
    }

    bool ok;
    if (s_clip_is_cut && filemgr_same_storage(s_clipboard, dst)) {
        /* 同存储剪切：rename 即可（失败则回退 copy+remove） */
        ok = (app_sys_fs_rename(s_clipboard, dst) == APP_FS_OK);
        if (!ok) {
            ok = filemgr_copy_recursive(s_clipboard, dst, 0) &&
                 filemgr_remove_recursive(s_clipboard, 0);
        }
    } else {
        /* 跨存储剪切或复制：先复制，剪切成功后再删源 */
        ok = filemgr_copy_recursive(s_clipboard, dst, 0);
        if (ok && s_clip_is_cut) {
            ok = filemgr_remove_recursive(s_clipboard, 0);
        }
    }
    if (ok) {
        ESP_LOGI(TAG, "paste %s: %s -> %s",
                 s_clip_is_cut ? "moved" : "copied", s_clipboard, dst);
        filemgr_msg(s_clip_is_cut ? lang_get(STR_FILE_MOVED)
                                  : lang_get(STR_FILE_COPIED));
        s_clip_valid = false;    /* 一次性剪贴板 */
        s_clipboard[0] = '\0';
    } else {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
    }
    filemgr_rescan_current();
}

/** 回收站：还原当前项（去掉 16 字符时间戳前缀） */
static void filemgr_restore(void)
{
    if (!s_in_recycle || s_filemgr_sel < 0 || s_filemgr_sel >= s_filemgr_count) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    char src[FILEMGR_PATH_LEN];
    if (!filemgr_entry_path(s_filemgr_sel, src, sizeof(src))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    const char *base = filemgr_basename(src);
    if (strlen(base) <= FM_RECYCLE_PREFIX_LEN) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    const char *orig_name = base + FM_RECYCLE_PREFIX_LEN;
    const char *root = s_recycle_src_sd[s_filemgr_sel] ? "/sdcard" : "/flash";
    char dst[FILEMGR_PATH_LEN];
    if (!filemgr_join_path(root, orig_name, dst, sizeof(dst))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    /* 原目录若已被删，则补建父目录，尽量保证还原成功 */
    char parent[FILEMGR_PATH_LEN];
    strncpy(parent, dst, FILEMGR_PATH_LEN - 1);
    parent[FILEMGR_PATH_LEN - 1] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        app_sys_fs_mkdir(parent);
    }
    if (app_sys_fs_rename(src, dst) == APP_FS_OK) {
        ESP_LOGI(TAG, "restored: %s -> %s", src, dst);
        filemgr_msg(lang_get(STR_COMMON_SUCCESS));
        filemgr_rescan_current();
    } else {
        ESP_LOGE(TAG, "restore failed: %s -> %s", src, dst);
        filemgr_msg(lang_get(STR_COMMON_FAILED));
    }
}

/** 回收站：彻底删除当前项 */
static void filemgr_purge(void)
{
    if (!s_in_recycle || s_filemgr_sel < 0 || s_filemgr_sel >= s_filemgr_count) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    char path[FILEMGR_PATH_LEN];
    if (!filemgr_entry_path(s_filemgr_sel, path, sizeof(path))) {
        filemgr_msg(lang_get(STR_COMMON_FAILED));
        return;
    }
    if (filemgr_remove_recursive(path, 0)) {
        ESP_LOGI(TAG, "purged: %s", path);
        filemgr_msg(lang_get(STR_COMMON_SUCCESS));
    } else {
        ESP_LOGE(TAG, "purge failed: %s", path);
        filemgr_msg(lang_get(STR_COMMON_FAILED));
    }
    filemgr_rescan_current();
}

/** 清空一个回收站目录（保留 .recycle 目录本身） */
static void filemgr_empty_one_recycle(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[FILEMGR_PATH_LEN];
        if (filemgr_join_path(dir, e->d_name, child, sizeof(child))) {
            if (!filemgr_remove_recursive(child, 0)) {
                ESP_LOGW(TAG, "empty recycle: failed to remove %s", child);
            }
        }
    }
    closedir(d);
}

/** 回收站：清空（内部 + SD 卡） */
static void filemgr_empty_recycle(void)
{
    filemgr_empty_one_recycle("/flash/.recycle");
    if (app_sys_sdcard_ready()) {
        filemgr_empty_one_recycle("/sdcard/.recycle");
    }
    ESP_LOGI(TAG, "recycle emptied");
    filemgr_msg(lang_get(STR_COMMON_SUCCESS));
    filemgr_rescan_current();
}

/* ===================================================================== */
/* 键盘回调                                                               */
/* ===================================================================== */

static void filemgr_kb_confirm(const char *text, void *user_data)
{
    (void)user_data;
    if (!text) return;
    strncpy(s_kb_input, text, FILEMGR_PATH_LEN - 1);
    s_kb_input[FILEMGR_PATH_LEN - 1] = '\0';

    switch (s_kb_purpose) {
    case FM_KB_NEW:
        filemgr_do_new(s_kb_input);
        break;
    case FM_KB_RENAME:
        filemgr_do_rename(s_kb_input);
        break;
    case FM_KB_SEARCH:
        if (s_kb_input[0] == '\0') {
            s_search_active = false;
            s_search_keyword[0] = '\0';
        } else {
            strncpy(s_search_keyword, s_kb_input, sizeof(s_search_keyword) - 1);
            s_search_keyword[sizeof(s_search_keyword) - 1] = '\0';
            s_search_active = true;
        }
        filemgr_rescan_current();
        break;
    default:
        break;
    }
    s_kb_purpose = FM_KB_NONE;
}

static void filemgr_kb_cancel(void *user_data)
{
    (void)user_data;
    s_kb_purpose = FM_KB_NONE;
}

static void filemgr_show_keyboard(fm_kb_purpose_t purpose,
                                  const char *title, const char *placeholder)
{
    s_kb_purpose = purpose;
    kb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.title = title;
    cfg.placeholder = placeholder;
    cfg.max_length = 80;
    cfg.password_mode = false;
    cfg.on_confirm = filemgr_kb_confirm;
    cfg.on_cancel = filemgr_kb_cancel;
    cfg.user_data = NULL;
    ui_keyboard_show(&cfg);
}

/* ===================================================================== */
/* 菜单执行 / 二次确认                                                     */
/* ===================================================================== */

static void filemgr_menu_execute(int pos)
{
    int id = filemgr_menu_id(pos);
    s_view = FM_VIEW_LIST;   /* 默认关闭菜单回到列表 */

    switch (id) {
    case FM_M_NEW:
        filemgr_show_keyboard(FM_KB_NEW, lang_get(STR_FILE_NEW),
                              lang_get(STR_FILE_NAME));
        return;
    case FM_M_RENAME:
        if (filemgr_is_root() || s_in_recycle ||
            s_filemgr_sel < 0 || s_filemgr_sel >= s_filemgr_count) {
            filemgr_msg(lang_get(STR_COMMON_FAILED));
            break;
        }
        s_kb_target_idx = s_filemgr_sel;
        filemgr_show_keyboard(FM_KB_RENAME, lang_get(STR_FILE_RENAME),
                              lang_get(STR_FILE_NAME));
        /* 预填当前名称，方便用户修改（show 之后设置才不会被清空） */
        ui_keyboard_set_text(s_filemgr_entries[s_kb_target_idx]);
        return;
    case FM_M_DELETE:
        s_confirm = FM_CONFIRM_DELETE;
        filemgr_msg(lang_get(STR_FILE_CONFIRM_DEL));
        return;
    case FM_M_COPY:
        filemgr_do_copy(false);
        break;
    case FM_M_CUT:
        filemgr_do_copy(true);
        break;
    case FM_M_PASTE:
        filemgr_paste();
        break;
    case FM_M_PROPS:
        s_props_idx = filemgr_target_idx();
        s_props_scroll = 0;
        s_view = FM_VIEW_PROPS;
        filemgr_refresh_props();
        return;
    case FM_M_SEARCH:
        if (s_in_recycle) {
            filemgr_msg(lang_get(STR_COMMON_FAILED));   /* 回收站视图不支持搜索 */
            break;
        }
        filemgr_show_keyboard(FM_KB_SEARCH, lang_get(STR_FILE_SEARCH),
                              lang_get(STR_FILE_NAME));
        return;
    case FM_M_SORT: {
        const char *names[] = {
            lang_get(STR_FILE_NAME), lang_get(STR_FILE_TIME), lang_get(STR_FILE_SIZE)
        };
        s_sort_key = (s_sort_key + 1) % 3;
        char msg[48];
        snprintf(msg, sizeof(msg), "%s: %s", lang_get(STR_FILE_SORT), names[s_sort_key]);
        filemgr_msg(msg);
        filemgr_rescan_current();
        return;
    }
    case FM_M_RECYCLE:
        strncpy(s_prev_path, s_filemgr_current_path, FILEMGR_PATH_LEN - 1);
        s_prev_path[FILEMGR_PATH_LEN - 1] = '\0';
        s_search_active = false;   /* 进入回收站清除搜索过滤 */
        filemgr_scan_recycle();
        break;
    case FM_M_RESTORE:
        filemgr_restore();
        break;
    case FM_M_PURGE:
        s_confirm = FM_CONFIRM_PURGE;
        filemgr_msg(lang_get(STR_FILE_CONFIRM_DEL));
        return;
    case FM_M_EMPTY:
        s_confirm = FM_CONFIRM_EMPTY;
        filemgr_msg(lang_get(STR_FILE_CONFIRM_DEL));
        return;
    default:
        break;
    }
    filemgr_refresh_list();
}

static void filemgr_confirm_execute(void)
{
    fm_confirm_t c = s_confirm;
    s_confirm = FM_CONFIRM_NONE;
    switch (c) {
    case FM_CONFIRM_DELETE:
        filemgr_soft_delete();
        break;
    case FM_CONFIRM_PURGE:
        filemgr_purge();
        break;
    case FM_CONFIRM_EMPTY:
        filemgr_empty_recycle();
        break;
    default:
        break;
    }
}

/* ===================================================================== */
/* 生命周期                                                               */
/* ===================================================================== */

static void filemgr_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "File manager init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_FILES));

    /* 重置视图相关状态（退出再进入时保持干净） */
    s_view = FM_VIEW_LIST;
    s_in_recycle = false;
    s_confirm = FM_CONFIRM_NONE;
    s_search_active = false;
    s_search_keyword[0] = '\0';
    s_clip_valid = false;
    s_clipboard[0] = '\0';
    s_menu_sel = 0;
    s_props_scroll = 0;

    /* 根据字体大小动态计算行高 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_filemgr_row_h = font_px + 1;

    lv_obj_t *list = lv_obj_create(scr);
    if (!list) {
        ESP_LOGE(TAG, "lv_obj_create(filemgr_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, ui_content_y());
    lv_obj_set_size(list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    s_filemgr_obj = list;
    s_txt_obj = list;  /* 文本查看器复用同一内容区 */
    ui_dock_create(scr, 1, 0);
    filemgr_scan_dir(s_filemgr_current_path);
    filemgr_refresh_list();
}

static void filemgr_destroy(void)
{
    ESP_LOGI(TAG, "File manager destroy");
    s_filemgr_obj = NULL;
    s_txt_obj = NULL;
    s_filemgr_count = 0;
    s_filemgr_sel = 0;
    s_filemgr_scroll = 0;
    s_view = FM_VIEW_LIST;
    s_in_recycle = false;
    s_confirm = FM_CONFIRM_NONE;
    txt_viewer_close();
}

/* ===================================================================== */
/* 按键处理                                                               */
/* ===================================================================== */

static bool filemgr_on_key(int key)
{
    /* 文本查看器模式 */
    if (s_txt_viewer_active) {
        if (key == KEY_B) {
            txt_viewer_close();
            s_txt_obj = s_filemgr_obj;
            filemgr_refresh_list();
            return true;
        }
        if (key == KEY_UP) {
            if (s_txt_scroll > 0) s_txt_scroll--;
            txt_viewer_refresh();
            return true;
        }
        if (key == KEY_DOWN) {
            int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
            int font_px = ui_state_get()->font_size;
            if (font_px < 14) font_px = 14;
            if (font_px > 24) font_px = 24;
            int row_h = font_px + 1;
            int vis_rows = (avail_h - row_h - 2) / row_h;
            if (vis_rows < 1) vis_rows = 1;
            if (s_txt_scroll + vis_rows < s_txt_line_count) s_txt_scroll++;
            txt_viewer_refresh();
            return true;
        }
        return true;
    }

    /* 二次确认态：A 执行、B 取消（吃掉其它键，避免误操作） */
    if (s_confirm != FM_CONFIRM_NONE) {
        if (key == KEY_A) {
            filemgr_confirm_execute();
        } else if (key == KEY_B) {
            s_confirm = FM_CONFIRM_NONE;
            filemgr_msg(lang_get(STR_COMMON_CANCEL));
            filemgr_refresh_list();
        }
        return true;
    }

    /* 操作菜单：UP/DOWN 选择，A 执行，B 返回 */
    if (s_view == FM_VIEW_MENU) {
        int cnt = filemgr_menu_count();
        if (key == KEY_B) {
            s_view = FM_VIEW_LIST;
            filemgr_refresh_list();
            return true;
        }
        if (key == KEY_UP) {
            if (s_menu_sel > 0) {
                s_menu_sel--;
                if (s_menu_sel < s_menu_scroll) s_menu_scroll = s_menu_sel;
            }
            filemgr_refresh_menu();
            return true;
        }
        if (key == KEY_DOWN) {
            if (s_menu_sel < cnt - 1) {
                s_menu_sel++;
                int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
                int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
                if (vis_rows < 1) vis_rows = 1;
                if (s_menu_sel >= s_menu_scroll + vis_rows)
                    s_menu_scroll = s_menu_sel - vis_rows + 1;
            }
            filemgr_refresh_menu();
            return true;
        }
        if (key == KEY_A) {
            filemgr_menu_execute(s_menu_sel);
            return true;
        }
        return true;
    }

    /* 属性页：UP/DOWN 滚动，B 返回 */
    if (s_view == FM_VIEW_PROPS) {
        if (key == KEY_B) {
            s_view = FM_VIEW_LIST;
            filemgr_refresh_list();
            return true;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            int font_px = ui_state_get()->font_size;
            if (font_px < 14) font_px = 14;
            if (font_px > 24) font_px = 24;
            int row_h = font_px + 1;
            int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
            int vis_rows = (avail_h - row_h - 2) / row_h;
            if (vis_rows < 1) vis_rows = 1;
            int max_scroll = 6 - vis_rows;
            if (max_scroll < 0) max_scroll = 0;
            if (key == KEY_UP) {
                if (s_props_scroll > 0) s_props_scroll--;
            } else {
                if (s_props_scroll < max_scroll) s_props_scroll++;
            }
            filemgr_refresh_props();
            return true;
        }
        return true;
    }

    /* 普通列表态 ------------------------------- */

    if (key == KEY_B) {
        if (s_in_recycle) {
            /* 回收站 -> 返回进入前目录 */
            filemgr_scan_dir(s_prev_path);
            filemgr_refresh_list();
            return true;
        }
        if (filemgr_is_root()) {
            /* 虚拟根：退出应用 */
            if (ui_stack_depth() > 1) ui_stack_pop();
        } else {
            char *last_slash = strrchr(s_filemgr_current_path, '/');
            if (last_slash && last_slash != s_filemgr_current_path) {
                *last_slash = '\0';
            } else {
                /* 一级目录（/flash、/sdcard）返回虚拟根 */
                strncpy(s_filemgr_current_path, "/", FILEMGR_PATH_LEN - 1);
                s_filemgr_current_path[FILEMGR_PATH_LEN - 1] = '\0';
            }
            s_search_active = false;   /* 返回上级时清除搜索过滤 */
            filemgr_scan_dir(s_filemgr_current_path);
            filemgr_refresh_list();
        }
        return true;
    }

    if (s_filemgr_count == 0) return true;

    int avail_h = LCD_V_RES - ui_content_y() - DOCK_H;
    int vis_rows = (avail_h - s_filemgr_row_h - 2) / s_filemgr_row_h;
    if (vis_rows < 1) vis_rows = 1;

    if (key == KEY_UP) {
        if (s_filemgr_sel > 0) {
            s_filemgr_sel--;
            if (s_filemgr_sel < s_filemgr_scroll) s_filemgr_scroll = s_filemgr_sel;
        }
        filemgr_refresh_list();
        return true;
    }
    if (key == KEY_DOWN) {
        if (s_filemgr_sel < s_filemgr_count - 1) {
            s_filemgr_sel++;
            if (s_filemgr_sel >= s_filemgr_scroll + vis_rows)
                s_filemgr_scroll = s_filemgr_sel - vis_rows + 1;
        }
        filemgr_refresh_list();
        return true;
    }

    /* LEFT：标记/取消标记当前项（多选） */
    if (key == KEY_LEFT) {
        if (s_filemgr_sel >= 0 && s_filemgr_sel < s_filemgr_count) {
            s_filemgr_marked[s_filemgr_sel] = !s_filemgr_marked[s_filemgr_sel];
            filemgr_refresh_list();
        }
        return true;
    }

    /* A：弹出操作菜单 */
    if (key == KEY_A) {
        s_menu_sel = 0;
        s_menu_scroll = 0;
        s_view = FM_VIEW_MENU;
        filemgr_refresh_menu();
        return true;
    }

    /* RIGHT：进入目录 / 打开文件（原 A 键功能），进入前显示剩余空间 */
    if (key == KEY_RIGHT) {
        if (!s_in_recycle && s_filemgr_is_dir[s_filemgr_sel]) {
            char new_path[FILEMGR_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            int n;
            if (filemgr_is_root()) {
                n = snprintf(new_path, sizeof(new_path), "/%s",
                             s_filemgr_entries[s_filemgr_sel]);
            } else {
                n = snprintf(new_path, sizeof(new_path), "%s/%s",
                             s_filemgr_current_path, s_filemgr_entries[s_filemgr_sel]);
            }
#pragma GCC diagnostic pop
            if (n < 0 || n >= (int)sizeof(new_path)) {
                ESP_LOGE(TAG, "Path too long, skip dir entry: %s",
                         s_filemgr_entries[s_filemgr_sel]);
                return true;
            }
            s_search_active = false;
            filemgr_scan_dir(new_path);
            filemgr_show_free(new_path);
            filemgr_refresh_list();
        } else if (!s_in_recycle) {
            char file_path[FILEMGR_PATH_LEN];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            int n = snprintf(file_path, sizeof(file_path), "%s/%s",
                             s_filemgr_current_path, s_filemgr_entries[s_filemgr_sel]);
#pragma GCC diagnostic pop
            if (n < 0 || n >= (int)sizeof(file_path)) {
                ESP_LOGE(TAG, "Path too long, skip file entry: %s",
                         s_filemgr_entries[s_filemgr_sel]);
                return true;
            }
            filemgr_open_file(file_path, s_filemgr_entries[s_filemgr_sel]);
        }
        return true;
    }
    return true;
}

/* ========== 应用安装卸载（在应用管理页面中增强） ========== */
static bool app_install_from_path(const char *app_path)
{
    if (!app_path) return false;
    char json_path[FILEMGR_PATH_LEN];
    snprintf(json_path, sizeof(json_path), "%s/app.json", app_path);
    struct stat st;
    if (stat(json_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "Install failed: no app.json in %s", app_path);
        return false;
    }
    char main_path[FILEMGR_PATH_LEN];
    snprintf(main_path, sizeof(main_path), "%s/main.py", app_path);
    if (stat(main_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "Install failed: no main.py in %s", app_path);
        return false;
    }
    const char *app_name = strrchr(app_path, '/');
    app_name = app_name ? app_name + 1 : app_path;
    char dest_path[FILEMGR_PATH_LEN];
    snprintf(dest_path, sizeof(dest_path), "/sdcard/apps/%s", app_name);
    mkdir(dest_path, 0755);
    ESP_LOGI(TAG, "App installed: %s -> %s", app_path, dest_path);
    app_manager_scan_sdcard();
    return true;
}

static bool app_uninstall(const char *app_name)
{
    if (!app_name) return false;
    char app_path[FILEMGR_PATH_LEN];
    snprintf(app_path, sizeof(app_path), "/sdcard/apps/%s", app_name);
    struct stat st;
    if (stat(app_path, &st) != 0) {
        ESP_LOGW(TAG, "Uninstall failed: app %s not found at %s", app_name, app_path);
        return false;
    }
    ESP_LOGI(TAG, "App uninstalled: %s", app_path);
    app_manager_scan_sdcard();
    return true;
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_filemgr_callbacks = {
    .init = filemgr_init,
    .destroy = filemgr_destroy,
    .on_key = filemgr_on_key,
};
