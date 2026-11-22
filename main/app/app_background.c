/**
 * @file app_background.c
 * @brief 小喵桌面 —— 后台模块页面（任务列表 / 服务开关 / 通知中心）
 *
 * 架构说明：
 *   - 独立应用文件，导出旧版 4 回调结构的 g_background_callbacks（参考 app_list.c）。
 *   - 页面骨架由 app_sys_api.h 的 app_sys_page_create() 创建（内部已建 statusbar），
 *     本文件不重复创建 statusbar；仅补充标签栏、列表容器与 dock。
 *   - 数据来自两个既有底座：bg_tasks（任务注册表）与 notifications（通知中心），
 *     统一经 snapshot 快照渲染；快照缓冲为 static，避免栈上大数组。
 *   - 三个标签页：任务(0)/服务(1)/通知(2)，LEFT/RIGHT 切换，B 返回。
 *
 * 注意：
 *   - notification_t 快照携带物理槽位 id，"单条已读"通过 notifications_mark_read(id)
 *     精确标记当前条（非整页），见 bg_notify_activate()。
 */
#include "app_sys_api.h"      /* 门面：app_sys_page_create / nav / theme / list */
#include "app_builtin.h"      /* 跳转目标页面回调（g_music_callbacks 等） */
#include "app_manager.h"
#include "ui_framework.h"     /* page_callbacks_t / key_type_t / lvgl / 主题 */
#include "bg_tasks.h"         /* 后台任务注册表 */
#include "notifications.h"    /* 通知中心 */
#include "lang/lang.h"
#include "fonts/lv_freetype_font.h"  /* lv_font_cn_get() 中文显示 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "APP_BG";

/* ========== 标签页与常量 ========== */
typedef enum {
    BG_TAB_TASKS = 0,   /* 后台任务 */
    BG_TAB_SERVICES,    /* 后台服务 */
    BG_TAB_NOTIFY,      /* 通知中心 */
    BG_TAB_COUNT
} bg_tab_t;

#define BG_NOTIFY_MAX    12     /* 与 notifications 环形缓冲容量一致 */
#define BG_ACTION_MAX    4      /* 单任务最多操作项（暂停/继续/取消/重试） */
#define BG_TAB_BAR_H     16     /* 标签栏高度 */
#define BG_DOCK_H        DOCK_H

/* 任务操作动作 */
typedef enum {
    BG_ACT_PAUSE = 0,
    BG_ACT_RESUME,
    BG_ACT_CANCEL,
    BG_ACT_RETRY
} bg_action_t;

/* ========== 静态对象与状态 ========== */
static lv_obj_t *s_root = NULL;     /* app_sys_page_create 返回的 screen（复用，不重复分配） */
static lv_obj_t *s_tabbar = NULL;   /* 标签栏 */
static lv_obj_t *s_content = NULL;  /* 列表内容区 */
static lv_obj_t *s_dock = NULL;     /* 底部页面指示器 */

static int s_tab = BG_TAB_TASKS;                 /* 当前标签页 */
static int s_sel[BG_TAB_COUNT]   = {0, 0, 0};    /* 各页选中索引 */
static int s_scroll[BG_TAB_COUNT] = {0, 0, 0};   /* 各页滚动偏移（行） */
static int s_content_h = 0;                      /* 内容区可用高度 */

/* 快照缓冲（static，避免栈上大数组） */
static bg_task_t s_tasks[BG_TASK_MAX_SLOTS];
static int s_task_count = 0;
static notification_t s_notifs[BG_NOTIFY_MAX];
static int s_notif_count = 0;

/* 任务操作菜单（浮层） */
static bool s_action_open = false;
static int  s_action_sel = 0;
static int  s_action_count = 0;
static int  s_action_items[BG_ACTION_MAX];

/* ========== 服务定义 ========== */
/*
 * 多数服务为常驻（无对应 bg task 可查），显示为"常驻"且不可切换；
 * 有 bg task 的类型可通过 bg_tasks_find() 查到实时状态，A 键做暂停/继续。
 */
typedef struct {
    const char *name;        /* 显示名（短，适配 160px） */
    bg_task_type_t type;     /* 关联的后台任务类型；(bg_task_type_t)-1 表示查不到，常驻 */
    bool toggleable;         /* 是否允许 A 键暂停/继续 */
} bg_service_def_t;

static const bg_service_def_t s_services[] = {
    { "音乐后台播放", BG_TASK_MUSIC,      true  },
    { "蓝牙服务",     BG_TASK_BT_CONNECT,  true  },
    { "WebREPL",     (bg_task_type_t)-1,  false },
    { "LED常驻",     BG_TASK_LED_ANIM,    true  },
    { "网络时间同步", (bg_task_type_t)-1,  false },
    { "OTA检查",     BG_TASK_OTA,         true  },
};
#define BG_SERVICE_COUNT ((int)(sizeof(s_services) / sizeof(s_services[0])))

/* ========== 自启动管理 ==========
 * 每个服务一个开机自启动开关，NVS 持久化（namespace "app_bg"，key bg_auto<idx>）。
 * 服务页 A 键切换；真正的开机拉起由 main 启动流程按 s_autostart 决定（P2 能力，
 * 当前以开关状态持久化 + UI 管理为交付）。 */
#define BG_NVS_NS        "app_bg"
#define BG_NVS_KEY_AUTO  "bg_auto%d"
static bool s_autostart[BG_SERVICE_COUNT] = {false};

/* ========== 内部工具 ========== */

/** 从 NVS 载入全部服务的自启动开关（缺省关闭） */
static void bg_autostart_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(BG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < BG_SERVICE_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), BG_NVS_KEY_AUTO, i);
        int32_t v = 0;
        nvs_get_i32(h, key, &v);
        s_autostart[i] = (v != 0);
    }
    nvs_close(h);
}

/** 保存单个服务的自启动开关 */
static void bg_autostart_save(int idx)
{
    if (idx < 0 || idx >= BG_SERVICE_COUNT) return;
    nvs_handle_t h;
    if (nvs_open(BG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    snprintf(key, sizeof(key), BG_NVS_KEY_AUTO, idx);
    nvs_set_i32(h, key, s_autostart[idx] ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/** 刷新两份快照：任务页和服务页共用任务快照 */
static void bg_refresh_snapshots(void)
{
    s_task_count = bg_tasks_snapshot(s_tasks, BG_TASK_MAX_SLOTS);
    if (s_task_count < 0) s_task_count = 0;
    if (s_task_count > BG_TASK_MAX_SLOTS) s_task_count = BG_TASK_MAX_SLOTS;

    s_notif_count = notifications_snapshot(s_notifs, BG_NOTIFY_MAX);
    if (s_notif_count < 0) s_notif_count = 0;
    if (s_notif_count > BG_NOTIFY_MAX) s_notif_count = BG_NOTIFY_MAX;
}

/** 按类型从任务快照取状态，查不到视为空闲 */
static bg_task_state_t bg_task_state_of_type(bg_task_type_t type)
{
    for (int i = 0; i < s_task_count; i++) {
        if (s_tasks[i].type == type) return s_tasks[i].state;
    }
    return BG_TASK_STATE_IDLE;
}

/** 列表字体：160x128 小屏限制在 14~16px，保证双行任务项放得下 */
static int bg_list_font_px(void)
{
    ui_state_t *st = ui_state_get();
    int px = st ? st->font_size : 14;
    if (px < 14) px = 14;
    if (px > 16) px = 16;
    return px;
}

/** 行高：任务为双行，服务/通知为单行 */
static int bg_row_h(bg_tab_t tab)
{
    int font_px = bg_list_font_px();
    return (tab == BG_TAB_TASKS) ? (font_px + 11) : (font_px + 4);
}

/** 当前页总行数 */
static int bg_total_rows(void)
{
    switch (s_tab) {
    case BG_TAB_TASKS:    return s_task_count;
    case BG_TAB_SERVICES: return BG_SERVICE_COUNT;
    case BG_TAB_NOTIFY:   return s_notif_count + 2; /* 末尾两行：全部已读 / 清空 */
    default:              return 0;
    }
}

/** 确保选中项在可见范围内 */
static void bg_ensure_visible(int total, int row_h)
{
    int vis = row_h > 0 ? (s_content_h / row_h) : 1;
    if (vis < 1) vis = 1;
    int idx = s_tab;
    if (s_sel[idx] < s_scroll[idx]) s_scroll[idx] = s_sel[idx];
    if (s_sel[idx] >= s_scroll[idx] + vis) s_scroll[idx] = s_sel[idx] - vis + 1;
    if (s_scroll[idx] > total - vis) s_scroll[idx] = total - vis;
    if (s_scroll[idx] < 0) s_scroll[idx] = 0;
}

/* ========== 渲染：标签栏 / dock ========== */

static void bg_render_tabbar(void)
{
    if (!s_tabbar) return;
    lv_obj_clean(s_tabbar);
    const theme_colors_t *colors = ui_theme_colors();
    const char *titles[BG_TAB_COUNT] = {
        lang_get(STR_BG_TASKS), lang_get(STR_BG_SERVICES), lang_get(STR_BG_NOTIFY)
    };
    int tab_w = LCD_H_RES / BG_TAB_COUNT;

    for (int i = 0; i < BG_TAB_COUNT; i++) {
        lv_obj_t *cell = lv_obj_create(s_tabbar);
        if (!cell) continue;
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, i * tab_w, 0);
        lv_obj_set_size(cell, tab_w, BG_TAB_BAR_H);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_tab && colors) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }

        lv_obj_t *lbl = lv_label_create(cell);
        if (!lbl) continue;
        lv_label_set_text(lbl, titles[i]);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(bg_list_font_px()), 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(i == s_tab ? colors->text : colors->text_dim), 0);
        lv_obj_set_width(lbl, tab_w - 2);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }
}

static void bg_render_dock(void)
{
    if (s_dock) {
        lv_obj_del(s_dock);
        s_dock = NULL;
    }
    if (s_root) {
        s_dock = ui_dock_create(s_root, BG_TAB_COUNT, s_tab);
    }
}

/* ========== 渲染：统一的行构造 ========== */

static lv_obj_t *bg_create_row(int y, int row_h, bool selected)
{
    lv_obj_t *row = lv_obj_create(s_content);
    if (!row) {
        ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return NULL;
    }
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_size(row, LCD_H_RES, row_h);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    const theme_colors_t *colors = ui_theme_colors();
    if (selected && colors) {
        lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    }
    return row;
}

static void bg_set_label(lv_obj_t *label, const char *text, uint32_t color, bool chinese)
{
    if (!label) return;
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (chinese) {
        lv_obj_set_style_text_font(label, lv_font_cn_get(bg_list_font_px()), 0);
    }
}

/* ========== 渲染：任务页 ========== */

static void bg_render_tasks(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    int row_h = bg_row_h(BG_TAB_TASKS);

    if (s_task_count <= 0) {
        lv_obj_t *empty = lv_label_create(s_content);
        if (empty && colors) {
            lv_label_set_text(empty, lang_get(STR_BG_EMPTY));
            lv_obj_set_style_text_font(empty, lv_font_cn_get(bg_list_font_px()), 0);
            lv_obj_set_style_text_color(empty, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        }
        return;
    }

    bg_ensure_visible(s_task_count, row_h);
    int vis = s_content_h / row_h;
    char buf[48];

    for (int i = s_scroll[s_tab]; i < s_task_count; i++) {
        int row_idx = i - s_scroll[s_tab];
        if (row_idx >= vis) break;
        bg_task_t *t = &s_tasks[i];
        bool selected = (i == s_sel[s_tab]);
        lv_obj_t *row = bg_create_row(row_idx * row_h, row_h, selected);
        if (!row) break;

        /* 第一行：类型名 + 标题（左），状态（右） */
        snprintf(buf, sizeof(buf), "%s %s", bg_tasks_type_name(t->type), t->title);
        lv_obj_t *name = lv_label_create(row);
        bg_set_label(name, buf, colors->text, true);
        if (name) {
            lv_obj_set_width(name, LCD_H_RES - 44);
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 3, -5);
        }

        lv_obj_t *state = lv_label_create(row);
        bg_set_label(state, bg_tasks_state_name(t->state), colors->text_dim, true);
        if (state) {
            lv_obj_align(state, LV_ALIGN_RIGHT_MID, -3, -5);
        }

        /* 第二行：进度条 + 百分比 */
        lv_obj_t *bar = lv_bar_create(row);
        if (bar) {
            lv_obj_remove_style_all(bar);
            lv_obj_set_pos(bar, 3, bg_list_font_px() + 3);
            lv_obj_set_size(bar, LCD_H_RES - 44, 4);
            lv_obj_set_style_radius(bar, 2, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(colors->sel_border), LV_PART_INDICATOR);
            int pct = t->progress;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            lv_bar_set_value(bar, pct, LV_ANIM_OFF);
        }

        snprintf(buf, sizeof(buf), "%d%%", t->progress);
        lv_obj_t *pct_lbl = lv_label_create(row);
        bg_set_label(pct_lbl, buf, colors->text_dim, false);
        if (pct_lbl) {
            lv_obj_align(pct_lbl, LV_ALIGN_RIGHT_MID, -3, 4);
        }
    }
}

/* ========== 渲染：服务页 ========== */

static void bg_render_services(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    int row_h = bg_row_h(BG_TAB_SERVICES);

    bg_ensure_visible(BG_SERVICE_COUNT, row_h);
    int vis = s_content_h / row_h;

    for (int i = s_scroll[s_tab]; i < BG_SERVICE_COUNT; i++) {
        int row_idx = i - s_scroll[s_tab];
        if (row_idx >= vis) break;
        const bg_service_def_t *svc = &s_services[i];
        bool selected = (i == s_sel[s_tab]);
        lv_obj_t *row = bg_create_row(row_idx * row_h, row_h, selected);
        if (!row) break;

        lv_obj_t *name = lv_label_create(row);
        bg_set_label(name, svc->name, colors->text, true);
        if (name) {
            lv_obj_set_width(name, LCD_H_RES - 60);
            lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, 0);
        }

        char status[24];
        uint32_t status_color = colors->text_dim;
        int id = ((int)svc->type >= 0) ? bg_tasks_find(svc->type) : -1;
        if (id >= 0) {
            const char *sn = bg_tasks_state_name(bg_task_state_of_type(svc->type));
            snprintf(status, sizeof(status), "%s", sn);
            status_color = colors->text;
        } else {
            /* 查不到状态的常驻服务 */
            snprintf(status, sizeof(status), "%s", "常驻");
        }
        /* 附加自启动标记（自启动管理：A 键切换，NVS 持久化） */
        {
            size_t len = strlen(status);
            snprintf(status + len, sizeof(status) - len, " %s",
                     s_autostart[i] ? "自启" : "-");
            if (s_autostart[i]) status_color = colors->text;
        }
        lv_obj_t *state = lv_label_create(row);
        bg_set_label(state, status, status_color, true);
        if (state) {
            lv_obj_align(state, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
}

/* ========== 渲染：通知页 ========== */

static void bg_render_notify(void)
{
    const theme_colors_t *colors = ui_theme_colors();
    int row_h = bg_row_h(BG_TAB_NOTIFY);
    int total = s_notif_count + 2;   /* 末尾：全部已读 / 清空通知 */
    int top_pad = 0;

    /* 空通知提示（不参与导航，仅占位显示） */
    if (s_notif_count <= 0) {
        lv_obj_t *empty = lv_label_create(s_content);
        if (empty && colors) {
            lv_label_set_text(empty, lang_get(STR_NOTIFY_EMPTY));
            lv_obj_set_style_text_font(empty, lv_font_cn_get(bg_list_font_px()), 0);
            lv_obj_set_style_text_color(empty, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 2);
        }
        top_pad = row_h;
    }

    bg_ensure_visible(total, row_h);
    int avail = s_content_h - top_pad;
    int vis = avail > 0 ? (avail / row_h) : 1;
    const char *actions[2] = { lang_get(STR_NOTIFY_READALL), lang_get(STR_NOTIFY_CLEAR) };
    char buf[48];

    for (int i = s_scroll[s_tab]; i < total; i++) {
        int row_idx = i - s_scroll[s_tab];
        if (row_idx >= vis) break;
        bool selected = (i == s_sel[s_tab]);
        lv_obj_t *row = bg_create_row(top_pad + row_idx * row_h, row_h, selected);
        if (!row) break;

        if (i < s_notif_count) {
            /* 通知行：类别图标 + 已读标记 + 标题 */
            notification_t *n = &s_notifs[i];
            char icon[2] = { notifications_category_icon(n->category), '\0' };
            lv_obj_t *icon_lbl = lv_label_create(row);
            if (icon_lbl) {
                lv_label_set_text(icon_lbl, icon);
                lv_obj_set_style_text_color(icon_lbl, lv_color_hex(colors->text_dim), 0);
                lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 3, 0);
            }

            snprintf(buf, sizeof(buf), "%s", n->title);
            lv_obj_t *title = lv_label_create(row);
            bg_set_label(title, buf, n->read ? colors->text_dim : colors->text, true);
            if (title) {
                lv_obj_set_width(title, LCD_H_RES - 52);
                lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_align(title, LV_ALIGN_LEFT_MID, 18, 0);
            }

            lv_obj_t *mark = lv_label_create(row);
            bg_set_label(mark, n->read ? "读" : "新", n->read ? colors->text_dim : colors->sel_border, true);
            if (mark) {
                lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -3, 0);
            }
        } else {
            /* 末尾操作行 */
            int act_idx = i - s_notif_count;
            lv_obj_t *lbl = lv_label_create(row);
            bg_set_label(lbl, actions[act_idx], colors->text, true);
            if (lbl) {
                lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
            }
        }
    }
}

/* ========== 渲染：任务操作菜单（浮层） ========== */

static void bg_render_action_menu(void)
{
    if (!s_action_open || !s_content) return;
    const theme_colors_t *colors = ui_theme_colors();
    int row_h = bg_list_font_px() + 4;
    int panel_h = s_action_count * row_h + 8;
    int panel_w = LCD_H_RES - 40;

    lv_obj_t *panel = lv_obj_create(s_content);
    if (!panel) return;
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 20, (s_content_h - panel_h) / 2);
    lv_obj_set_size(panel, panel_w, panel_h);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(colors->sel_border), 0);
    lv_obj_set_style_border_width(panel, 1, 0);

    const char *names[4] = {
        lang_get(STR_BG_PAUSE), lang_get(STR_BG_RESUME),
        lang_get(STR_BG_CANCEL), lang_get(STR_BG_RETRY)
    };
    for (int i = 0; i < s_action_count; i++) {
        lv_obj_t *item = lv_obj_create(panel);
        if (!item) continue;
        lv_obj_remove_style_all(item);
        lv_obj_set_pos(item, 0, 4 + i * row_h);
        lv_obj_set_size(item, panel_w, row_h);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_action_sel) {
            lv_obj_set_style_bg_color(item, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        }
        int act = s_action_items[i];
        lv_obj_t *lbl = lv_label_create(item);
        bg_set_label(lbl, names[act], colors->text, true);
        if (lbl) lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }
}

/** 重建当前标签页内容；操作菜单以浮层叠加 */
static void bg_rebuild(void)
{
    if (!s_content) return;
    lv_obj_clean(s_content);

    switch (s_tab) {
    case BG_TAB_TASKS:    bg_render_tasks();    break;
    case BG_TAB_SERVICES: bg_render_services(); break;
    case BG_TAB_NOTIFY:   bg_render_notify();   break;
    default: break;
    }
    bg_render_action_menu();
}

/* ========== 交互：跳转 / 操作 ========== */

/** 根据 jump_target 跳转到目标应用页；jump_target 为业务自定义，0~4 为本项目约定 */
static void bg_jump(int target)
{
    const page_callbacks_t *cb = NULL;
    const char *title = NULL;
    switch (target) {
    case 0: cb = &g_music_callbacks;   title = lang_get(STR_APP_MUSIC);  break;
    case 1: cb = &g_ota_callbacks;     title = lang_get(STR_OTA_IDLE);   break;
    case 2: cb = &g_store_callbacks;   title = lang_get(STR_APP_STORE);  break;
    case 3: cb = &g_filemgr_callbacks; title = lang_get(STR_APP_FILES);  break;
    case 4: cb = &g_settings_callbacks; title = lang_get(STR_SETTINGS);  break;
    default: return;   /* 无约定映射：不跳转 */
    }
    if (cb) app_sys_nav_push(PAGE_CUSTOM, cb, title);
}

/** 打开当前任务的动作菜单（按状态给出可用项） */
static void bg_open_action(void)
{
    if (s_task_count <= 0 || s_sel[s_tab] >= s_task_count) return;
    bg_task_state_t st = s_tasks[s_sel[s_tab]].state;
    s_action_count = 0;
    if (st == BG_TASK_STATE_RUNNING) {
        s_action_items[s_action_count++] = BG_ACT_PAUSE;
        s_action_items[s_action_count++] = BG_ACT_CANCEL;
    } else if (st == BG_TASK_STATE_PAUSED) {
        s_action_items[s_action_count++] = BG_ACT_RESUME;
        s_action_items[s_action_count++] = BG_ACT_CANCEL;
    } else {
        /* 空闲/完成/出错/取消：提供重试 */
        s_action_items[s_action_count++] = BG_ACT_RETRY;
    }
    s_action_sel = 0;
    s_action_open = true;
    bg_rebuild();
}

/** 执行选中的动作 */
static void bg_exec_action(void)
{
    if (!s_action_open || s_action_count <= 0) return;
    int act = s_action_items[s_action_sel];
    int id = bg_tasks_find(s_tasks[s_sel[s_tab]].type);
    if (id >= 0) {
        switch (act) {
        case BG_ACT_PAUSE:  bg_tasks_pause(id);  break;
        case BG_ACT_RESUME: bg_tasks_resume(id); break;
        case BG_ACT_CANCEL: bg_tasks_cancel(id); break;
        case BG_ACT_RETRY:
            bg_tasks_set_progress(id, 0, NULL);
            bg_tasks_set_state(id, BG_TASK_STATE_RUNNING);
            break;
        default: break;
        }
    } else {
        ESP_LOGW(TAG, "task id not found for action %d", act);
    }
    s_action_open = false;
    bg_refresh_snapshots();
    bg_rebuild();
}

/** 服务页 A 键：有 bg task 的项做暂停/继续切换 */
/** 服务页 A 键：切换该服务的开机自启动开关（NVS 持久化，见 s_autostart） */
static void bg_service_toggle(void)
{
    if (s_sel[s_tab] < 0 || s_sel[s_tab] >= BG_SERVICE_COUNT) return;
    int idx = s_sel[s_tab];
    s_autostart[idx] = !s_autostart[idx];
    bg_autostart_save(idx);
    ESP_LOGI(TAG, "service[%d] %s autostart=%d",
             idx, s_services[idx].name, (int)s_autostart[idx]);
    bg_rebuild();
}

/** 通知页 A 键：单条已读+跳转 / 全部已读 / 清空 */
static void bg_notify_activate(void)
{
    int total = s_notif_count + 2;
    if (s_sel[s_tab] < 0 || s_sel[s_tab] >= total) return;

    if (s_sel[s_tab] < s_notif_count) {
        /* 单条已读：notification_t 快照携带物理槽位 id，可精确标记当前这条 */
        int nid = s_notifs[s_sel[s_tab]].id;
        notifications_mark_read(nid);
        int jt = s_notifs[s_sel[s_tab]].jump_target;
        bg_refresh_snapshots();
        if (jt >= 0) {
            bg_jump(jt);   /* 跳转会重建屏幕，此处不 rebuild */
            return;
        }
        bg_rebuild();
    } else if (s_sel[s_tab] == s_notif_count) {
        notifications_mark_all_read();
        s_sel[s_tab] = 0;
        bg_refresh_snapshots();
        bg_rebuild();
    } else {
        notifications_clear();
        s_sel[s_tab] = 0;
        s_scroll[s_tab] = 0;
        bg_refresh_snapshots();
        bg_rebuild();
    }
}

/* ========== 生命周期回调 ========== */

static void background_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Background page init");

    /* 载入服务自启动开关（NVS，缺省关闭） */
    bg_autostart_load_all();

    /* app_sys_page_create 内部已创建 statusbar；复用同一 screen，避免重复分配 */
    if (!s_root) {
        s_root = app_sys_page_create(lang_get(STR_BACKGROUND));
        if (!s_root) {
            ESP_LOGE(TAG, "app_sys_page_create failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            return;
        }
    }
    lv_screen_load(s_root);

    /*
     * 统一清屏后重建"唯一"statusbar：
     * - 首次 app_sys_page_create 建的 statusbar 在此被清掉重建；
     * - 从桌面/子页面返回时，它们可能在本 screen 上遗留自己的 statusbar，
     *   一并清掉，避免状态栏叠加。destroy 已将 s_tabbar/s_content/s_dock 置 NULL。
     */
    lv_obj_clean(s_root);
    ui_statusbar_create(s_root);
    ui_statusbar_set_title(lang_get(STR_BACKGROUND));

    const theme_colors_t *colors = ui_theme_colors();
    if (colors) {
        lv_obj_set_style_bg_color(s_root, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    }

    int content_y = ui_content_y() + BG_TAB_BAR_H;
    s_content_h = LCD_V_RES - content_y - BG_DOCK_H;
    if (s_content_h < 0) s_content_h = 0;

    /* 标签栏（clean 后旧对象已失效，一律重建） */
    s_tabbar = lv_obj_create(s_root);
    if (s_tabbar) {
        lv_obj_remove_style_all(s_tabbar);
        lv_obj_set_pos(s_tabbar, 0, ui_content_y());
        lv_obj_set_size(s_tabbar, LCD_H_RES, BG_TAB_BAR_H);
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(s_tabbar, LV_OPA_TRANSP, 0);
    }

    /* 列表内容区 */
    s_content = lv_obj_create(s_root);
    if (!s_content) {
        ESP_LOGE(TAG, "lv_obj_create(content) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_content);
    lv_obj_set_pos(s_content, 0, content_y);
    lv_obj_set_size(s_content, LCD_H_RES, s_content_h);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);

    bg_render_tabbar();
    bg_render_dock();
    bg_refresh_snapshots();
    bg_rebuild();
}

static void background_activate(void)
{
    /* 从子页面返回或数据变化后，重新取快照并刷新（幂等） */
    if (!s_content) return;
    bg_refresh_snapshots();
    bg_rebuild();
}

static void background_destroy(void)
{
    ESP_LOGI(TAG, "Background page destroy");
    s_content = NULL;
    s_tabbar = NULL;
    s_dock = NULL;
    s_action_open = false;
    s_action_count = 0;
    s_sel[0] = s_sel[1] = s_sel[2] = 0;
    s_scroll[0] = s_scroll[1] = s_scroll[2] = 0;
    /* s_root 保留复用；不在此处 lv_obj_del，避免页面栈生命周期中误删正在显示的 screen */
}

static bool background_on_key(int key)
{
    /* 动作菜单优先消费按键 */
    if (s_action_open) {
        if (key == KEY_UP) {
            s_action_sel = (s_action_sel - 1 + s_action_count) % s_action_count;
            bg_rebuild();
            return true;
        }
        if (key == KEY_DOWN) {
            s_action_sel = (s_action_sel + 1) % s_action_count;
            bg_rebuild();
            return true;
        }
        if (key == KEY_A) {
            bg_exec_action();
            return true;
        }
        if (key == KEY_B) {
            s_action_open = false;
            bg_rebuild();
            return true;
        }
        return true;
    }

    if (key == KEY_B) {
        app_sys_nav_back();
        return true;
    }

    /* LEFT/RIGHT 切换标签页 */
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        int next = s_tab + (key == KEY_RIGHT ? 1 : -1);
        if (next < 0) next = BG_TAB_COUNT - 1;
        if (next >= BG_TAB_COUNT) next = 0;
        if (next != s_tab) {
            s_tab = next;
            s_sel[s_tab] = 0;
            s_scroll[s_tab] = 0;
            bg_render_tabbar();
            bg_render_dock();
            bg_refresh_snapshots();
            bg_rebuild();
        }
        return true;
    }

    if (key == KEY_UP || key == KEY_DOWN) {
        int total = bg_total_rows();
        if (total <= 0) return true;
        if (key == KEY_UP) {
            s_sel[s_tab] = (s_sel[s_tab] - 1 + total) % total;
        } else {
            s_sel[s_tab] = (s_sel[s_tab] + 1) % total;
        }
        bg_rebuild();
        return true;
    }

    if (key == KEY_A) {
        switch (s_tab) {
        case BG_TAB_TASKS:    bg_open_action();   break;
        case BG_TAB_SERVICES: bg_service_toggle(); break;
        case BG_TAB_NOTIFY:   bg_notify_activate(); break;
        default: break;
        }
        return true;
    }
    return true;
}

/* ========== 页面回调定义（全局可见，旧版 4 回调结构，参考 app_list.c） ========== */
const page_callbacks_t g_background_callbacks = {
    .init    = background_init,
    .activate = background_activate,
    .destroy = background_destroy,
    .on_key  = background_on_key,
};

/*
 * ========== 风险点 / 待对齐事项（供集成方参考） ==========
 * 1. notifications 单条已读：notification_t 已增加 id 字段（物理槽位 0~11），
 *    snapshot 时填充，bg_notify_activate() 用 notifications_mark_read(id) 精确标记
 *    当前条；"全部已读"仍用 notifications_mark_all_read()。
 * 2. app_sys_page_create() 实现仅创建 statusbar + screen，并未创建 titlebar/dock，
 *    且返回的是整个 screen（不是"内容区"），也未调用 lv_screen_load()。本文件已按
 *    真实实现处理：自行 lv_screen_load、补 dock、按 ui_content_y() 计算内容区。
 * 3. jump_target 为业务自定义整数，项目暂无统一约定；本文件暂用 0~4 映射到
 *    音乐/OTA/商店/文件/设置五个页面，其它值忽略。业务发布通知时需按此约定传值。
 * 4. 服务页状态来自 bg_tasks_find() 与任务快照；WebREPL/网络时间同步无对应
 *    bg task，显示"常驻"。服务页 A 键 = 切换该服务开机自启动（s_autostart，
 *    NVS "app_bg"/bg_auto<i> 持久化）；服务的暂停/继续在任务页动作菜单完成。
 * 5. 自启动管理（P2）：开关状态已持久化并可在服务页管理；开机按开关拉起
 *    服务的接入点在 main 启动流程，当前以开关管理为交付。
 */
