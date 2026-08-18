/**
 * @file app_settings_wifi.c
 * @brief WiFi设置二级页面 - STA/AP双模式，扫描、连接WiFi网络
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_wifi_settings_callbacks。
 *
 * v65改进：
 * 1. STA/AP双模式：第0行左右键切换
 * 2. STA模式：进入页面自动扫描，状态行A键手动重新扫描
 * 3. AP模式：显示SSID/密码/频道/连接数信息行
 * 4. 添加 esp_netif_create_default_wifi_sta/ap() 创建网络接口
 * 5. WiFi开关移到第1行，模式行独立
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "fonts/lv_freetype_font.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "APP_WIFI";

/* ========== WiFi状态 ========== */
#define MAX_NETWORKS 8
#define WIFI_SSID_MAX 32
#define AP_PASSWORD   "12345678"   /* AP热点密码（8位） */
#define AP_SSID       "Xiaomiao-AP"

typedef enum {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_IDLE,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

/* 用户选择的模式 */
typedef enum {
    WIFI_MODE_SEL_STA = 0,
    WIFI_MODE_SEL_AP,
    WIFI_MODE_SEL_MAX
} wifi_mode_sel_t;

static wifi_state_t s_wifi_state = WIFI_STATE_OFF;
static bool s_wifi_initialized = false;
static bool s_sta_netif_created = false;  /* STA netif 是否已创建 */
static bool s_ap_netif_created = false;   /* AP netif 是否已创建 */
static wifi_mode_sel_t s_mode_sel = WIFI_MODE_SEL_STA;
static int s_ap_sta_count = 0;            /* AP模式下已连接设备数 */

/* 扫描到的网络列表 */
typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    int rssi;
    uint8_t auth_mode;  /* 0=开放, 1=WEP, 2=WPA, 3=WPA2, 4=WPA3 */
} wifi_network_t;

static wifi_network_t s_networks[MAX_NETWORKS];
static int s_network_count = 0;
static int s_connected_idx = -1;  /* 当前连接的网络索引 */

/* ========== UI状态 ========== */
#define WIFI_ROW_MODE     0   /* 模式行（STA/AP切换） */
#define WIFI_ROW_SWITCH   1   /* WiFi开关行 */
#define WIFI_ROW_STATUS   2   /* 状态行（A键扫描） */
#define WIFI_ROW_NET_BASE 3   /* 网络列表起始行（STA）/ AP信息起始行 */
#define AP_INFO_ROWS      3   /* AP模式信息行数（名称/密码/频道） */

static lv_obj_t *s_wifi_list = NULL;
/* 标签数组容量：3固定行 + 8网络 + 3AP信息 */
static lv_obj_t *s_wifi_labels[WIFI_ROW_NET_BASE + MAX_NETWORKS + AP_INFO_ROWS + 2] = {0};
static lv_obj_t *s_wifi_switch = NULL;                    /* WiFi开关组件 */
static lv_obj_t *s_wifi_bars[MAX_NETWORKS] = {0};        /* 网络信号强度进度条 */
static int s_wifi_sel = 0;
static int s_wifi_scroll = 0;
static int s_wifi_vis_rows = 6;
static int s_wifi_row_h = 14;
static int s_wifi_total = 0;

/* ========== WiFi驱动初始化 ========== */
static void wifi_driver_init(void)
{
    if (s_wifi_initialized) return;

    /* 注意：esp_netif_init() 和 esp_event_loop_create_default() 已在 app_main 中初始化一次 */

    /* 创建STA和AP网络接口（各一次，模式切换时复用） */
    if (!s_sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        s_sta_netif_created = true;
        ESP_LOGI(TAG, "STA netif created");
    }
    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
        ESP_LOGI(TAG, "AP netif created");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s (DMA memory may be insufficient)", esp_err_to_name(ret));
        return;
    }
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);  /* 双模式，便于 STA/AP 切换 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        return;
    }

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi driver initialized (APSTA)");
}

/* ========== 应用当前所选模式 ========== */
static void wifi_apply_mode(void)
{
    if (!s_wifi_initialized) return;
    ui_state_t *st = ui_state_get();
    if (!st->wifi_on) return;

    if (s_mode_sel == WIFI_MODE_SEL_AP) {
        wifi_config_t ap_cfg = {
            .ap = {
                .ssid = AP_SSID,
                .ssid_len = strlen(AP_SSID),
                .password = AP_PASSWORD,
                .max_connection = 4,
                .channel = 1,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        esp_err_t ret = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AP config failed: %s", esp_err_to_name(ret));
        }
        ret = esp_wifi_set_mode(WIFI_MODE_AP);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Set AP mode failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "AP mode: %s / %s ch1", AP_SSID, AP_PASSWORD);
        }
    } else {
        esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Set STA mode failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "STA mode");
        }
    }
}

/* ========== WiFi扫描 ========== */
static void wifi_scan(void)
{
    if (s_mode_sel != WIFI_MODE_SEL_STA) return;
    if (s_wifi_state == WIFI_STATE_OFF) return;
    ui_state_t *st = ui_state_get();
    if (!st->wifi_on) return;

    s_wifi_state = WIFI_STATE_SCANNING;
    s_network_count = 0;

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        s_wifi_state = WIFI_STATE_IDLE;
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_NETWORKS) ap_count = MAX_NETWORKS;

    wifi_ap_record_t ap_records[MAX_NETWORKS];
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    for (int i = 0; i < ap_count && i < MAX_NETWORKS; i++) {
        strncpy(s_networks[i].ssid, (const char*)ap_records[i].ssid, WIFI_SSID_MAX);
        s_networks[i].ssid[WIFI_SSID_MAX] = '\0';
        s_networks[i].rssi = ap_records[i].rssi;
        s_networks[i].auth_mode = ap_records[i].authmode;
    }
    s_network_count = ap_count;
    s_wifi_state = WIFI_STATE_IDLE;

    ESP_LOGI(TAG, "WiFi scan complete: %d networks found", s_network_count);
}

/* ========== WiFi连接/断开 ========== */
static void wifi_connect(int idx)
{
    if (idx < 0 || idx >= s_network_count) return;
    if (s_mode_sel != WIFI_MODE_SEL_STA) return;
    if (s_wifi_state == WIFI_STATE_OFF) return;
    ui_state_t *st = ui_state_get();
    if (!st->wifi_on) return;

    s_wifi_state = WIFI_STATE_CONNECTING;
    s_connected_idx = -1;

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, s_networks[idx].ssid, WIFI_SSID_MAX);
    /* 密码为空（开放网络）或后续通过输入框获取 */

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set config failed: %s", esp_err_to_name(ret));
        s_wifi_state = WIFI_STATE_IDLE;
        return;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi connect failed: %s", esp_err_to_name(ret));
        s_wifi_state = WIFI_STATE_IDLE;
        return;
    }

    s_connected_idx = idx;
    s_wifi_state = WIFI_STATE_CONNECTED;
    ESP_LOGI(TAG, "Connecting to: %s", s_networks[idx].ssid);
}

static void wifi_disconnect(void)
{
    if (s_wifi_state == WIFI_STATE_CONNECTED || s_wifi_state == WIFI_STATE_CONNECTING) {
        esp_wifi_disconnect();
        s_connected_idx = -1;
        s_wifi_state = WIFI_STATE_IDLE;
        ESP_LOGI(TAG, "WiFi disconnected");
    }
}

/* ========== WiFi开关（统一启用/禁用） ========== */
static void wifi_set_enabled(bool enable)
{
    ui_state_t *st = ui_state_get();
    st->wifi_on = enable;

    if (enable) {
        if (!s_wifi_initialized) {
            wifi_driver_init();  /* init + start */
        } else {
            esp_wifi_start();
        }
        if (!s_wifi_initialized) {
            ESP_LOGE(TAG, "WiFi enable failed (init failed)");
            return;
        }
        wifi_apply_mode();
        if (s_mode_sel == WIFI_MODE_SEL_STA) {
            wifi_scan();
        }
    } else {
        if (s_wifi_initialized) {
            esp_wifi_stop();
        }
        s_network_count = 0;
        s_connected_idx = -1;
        s_wifi_state = WIFI_STATE_OFF;
        ESP_LOGI(TAG, "WiFi disabled");
    }
}

/* ========== 刷新UI ========== */
static void wifi_refresh_label(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_wifi_labels) / sizeof(s_wifi_labels[0]))) return;
    if (!s_wifi_labels[idx]) return;
    ui_state_t *st = ui_state_get();
    char buf[64];

    if (idx == WIFI_ROW_MODE) {
        /* 第0行：模式切换 */
        snprintf(buf, sizeof(buf), "%s", s_mode_sel == WIFI_MODE_SEL_STA ? "模式: STA" : "模式: AP");
    } else if (idx == WIFI_ROW_SWITCH) {
        /* 第1行：WiFi开关 — 文本只显示"WiFi"，开关由lv_switch组件管理 */
        snprintf(buf, sizeof(buf), "WiFi");
    } else if (idx == WIFI_ROW_STATUS) {
        /* 第2行：状态信息 */
        if (!st->wifi_on) {
            snprintf(buf, sizeof(buf), "WiFi已关闭");
        } else if (s_mode_sel == WIFI_MODE_SEL_AP) {
            snprintf(buf, sizeof(buf), "AP运行中 A=详情");
        } else if (s_wifi_state == WIFI_STATE_SCANNING) {
            snprintf(buf, sizeof(buf), "正在扫描...");
        } else if (s_wifi_state == WIFI_STATE_CONNECTING) {
            snprintf(buf, sizeof(buf), "正在连接...");
        } else if (s_wifi_state == WIFI_STATE_CONNECTED && s_connected_idx >= 0) {
            snprintf(buf, sizeof(buf), "已连接: %s", s_networks[s_connected_idx].ssid);
        } else {
            snprintf(buf, sizeof(buf), "已扫%d个 A=扫描", s_network_count);
        }
    } else if (idx >= WIFI_ROW_NET_BASE) {
        if (s_mode_sel == WIFI_MODE_SEL_AP) {
            /* AP模式信息行 */
            int ap_idx = idx - WIFI_ROW_NET_BASE;
            if (ap_idx == 0) snprintf(buf, sizeof(buf), "名称: %s", AP_SSID);
            else if (ap_idx == 1) snprintf(buf, sizeof(buf), "密码: %s", AP_PASSWORD);
            else if (ap_idx == 2) {
                /* 查询AP实际连接数 */
                wifi_sta_list_t sta_list;
                memset(&sta_list, 0, sizeof(sta_list));
                if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
                    s_ap_sta_count = sta_list.num;
                }
                snprintf(buf, sizeof(buf), "频道:1 连接:%d", s_ap_sta_count);
            }
            else buf[0] = '\0';
        } else {
            /* STA模式网络列表行 */
            int net_idx = idx - WIFI_ROW_NET_BASE;
            if (net_idx < s_network_count) {
                const char *lock = "";
                if (s_networks[net_idx].auth_mode > 0) lock = "🔒";
                if (net_idx == s_connected_idx) {
                    snprintf(buf, sizeof(buf), "%s%s ✓", lock, s_networks[net_idx].ssid);
                } else {
                    snprintf(buf, sizeof(buf), "%s%s", lock, s_networks[net_idx].ssid);
                }
                /* 更新信号强度进度条 */
                if (net_idx < MAX_NETWORKS && s_wifi_bars[net_idx]) {
                    int bars = (s_networks[net_idx].rssi + 100) / 17;
                    if (bars < 0) bars = 0;
                    if (bars > 4) bars = 4;
                    lv_bar_set_value(s_wifi_bars[net_idx], bars * 25, LV_ANIM_OFF);
                }
            } else {
                buf[0] = '\0';
            }
        }
    } else {
        buf[0] = '\0';
    }
    lv_label_set_text(s_wifi_labels[idx], buf);
}

static void wifi_rebuild_visible(void)
{
    if (!s_wifi_list) return;
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(s_wifi_list);
    memset(s_wifi_labels, 0, sizeof(s_wifi_labels));
    memset(s_wifi_bars, 0, sizeof(s_wifi_bars));

    if (s_mode_sel == WIFI_MODE_SEL_AP) {
        s_wifi_total = WIFI_ROW_NET_BASE + AP_INFO_ROWS;  /* 模式+开关+状态+3行AP信息 */
        if (s_wifi_total < WIFI_ROW_NET_BASE) s_wifi_total = WIFI_ROW_NET_BASE;
    } else {
        s_wifi_total = WIFI_ROW_NET_BASE + s_network_count;  /* 模式+开关+状态+网络列表 */
        if (s_wifi_total < WIFI_ROW_NET_BASE) s_wifi_total = WIFI_ROW_NET_BASE;
    }

    /* 确保选中项在可见范围内 */
    if (s_wifi_sel < s_wifi_scroll) s_wifi_scroll = s_wifi_sel;
    if (s_wifi_sel >= s_wifi_scroll + s_wifi_vis_rows) {
        s_wifi_scroll = s_wifi_sel - s_wifi_vis_rows + 1;
    }
    if (s_wifi_scroll > s_wifi_total - s_wifi_vis_rows) {
        s_wifi_scroll = s_wifi_total - s_wifi_vis_rows;
    }
    if (s_wifi_scroll < 0) s_wifi_scroll = 0;

    for (int i = 0; i < s_wifi_vis_rows && (s_wifi_scroll + i) < s_wifi_total; i++) {
        int idx = s_wifi_scroll + i;
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_wifi_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_wifi_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (idx == s_wifi_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(st->font_size), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);
        s_wifi_labels[idx] = lbl;
        wifi_refresh_label(idx);

        /* 第1行：添加LVGL开关组件 */
        if (idx == WIFI_ROW_SWITCH) {
            lv_obj_t *sw = lv_switch_create(row);
            lv_obj_remove_style_all(sw);
            /* 开关背景 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->border), 0);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(sw, 8, 0);
            /* 开关指示器（开启时填充色） */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->text), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(sw, 8, LV_PART_INDICATOR);
            /* 开关旋钮 */
            lv_obj_set_style_bg_color(sw, lv_color_hex(colors->bg), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(sw, 6, LV_PART_KNOB);
            lv_obj_set_style_pad_all(sw, 2, LV_PART_KNOB);
            lv_obj_set_size(sw, 36, s_wifi_row_h - 4);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
            /* 设置开关状态 */
            if (st->wifi_on) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
            }
            s_wifi_switch = sw;
        }

        /* STA模式网络列表行：添加信号强度进度条 */
        if (s_mode_sel == WIFI_MODE_SEL_STA && idx >= WIFI_ROW_NET_BASE) {
            int net_idx = idx - WIFI_ROW_NET_BASE;
            if (net_idx < s_network_count && net_idx < MAX_NETWORKS) {
                lv_obj_t *bar = lv_bar_create(row);
                lv_obj_remove_style_all(bar);
                /* 进度条背景 */
                lv_obj_set_style_bg_color(bar, lv_color_hex(colors->border), 0);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(bar, 2, 0);
                /* 进度条指示器（填充部分） */
                lv_obj_set_style_bg_color(bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
                lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
                lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
                /* 位置：行右侧 */
                lv_obj_set_size(bar, 40, s_wifi_row_h - 6);
                lv_obj_align(bar, LV_ALIGN_RIGHT_MID, -6, 0);
                lv_bar_set_range(bar, 0, 100);
                lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                s_wifi_bars[net_idx] = bar;
            }
        }
    }
}

/* ========== 页面生命周期 ========== */
static void wifi_settings_init(void *data)
{
    ESP_LOGI(TAG, "WiFi settings init");
    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_statusbar_create(scr);
    ui_statusbar_set_title("WiFi设置");

    /* 计算行高和可见行数 */
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_wifi_row_h = font_px + 2;
    s_wifi_vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_wifi_row_h;
    if (s_wifi_vis_rows < 1) s_wifi_vis_rows = 1;

    s_wifi_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_pos(s_wifi_list, 0, ui_content_y());
    lv_obj_set_size(s_wifi_list, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    s_wifi_sel = 0;
    s_wifi_scroll = 0;

    /* 如果WiFi已打开，初始化驱动并按当前模式处理 */
    if (st->wifi_on) {
        if (!s_wifi_initialized) {
            wifi_driver_init();
        }
        wifi_apply_mode();
        if (s_mode_sel == WIFI_MODE_SEL_STA) {
            wifi_scan();   /* STA模式：进入页面自动扫描 */
        }
        if (!s_wifi_initialized) {
            ESP_LOGW(TAG, "WiFi not initialized (DMA不足?), 仅显示开关");
        }
    }

    wifi_rebuild_visible();
    ui_dock_create(scr, 1, 0);
}

static void wifi_settings_destroy(void)
{
    ESP_LOGI(TAG, "WiFi settings destroy");
    s_wifi_list = NULL;
    s_wifi_switch = NULL;
    memset(s_wifi_labels, 0, sizeof(s_wifi_labels));
    memset(s_wifi_bars, 0, sizeof(s_wifi_bars));
}

static bool wifi_settings_on_key(int key)
{
    ui_state_t *st = ui_state_get();

    if (key == KEY_B) {
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }

    if (key == KEY_UP) {
        s_wifi_sel = (s_wifi_sel - 1 + s_wifi_total) % s_wifi_total;
        wifi_rebuild_visible();
        return true;
    }
    if (key == KEY_DOWN) {
        s_wifi_sel = (s_wifi_sel + 1) % s_wifi_total;
        wifi_rebuild_visible();
        return true;
    }

    if (key == KEY_LEFT || key == KEY_RIGHT) {
        /* 第0行：切换 STA/AP 模式 */
        if (s_wifi_sel == WIFI_ROW_MODE) {
            s_mode_sel = (s_mode_sel == WIFI_MODE_SEL_STA) ? WIFI_MODE_SEL_AP : WIFI_MODE_SEL_STA;
            if (st->wifi_on && s_wifi_initialized) {
                wifi_apply_mode();
                if (s_mode_sel == WIFI_MODE_SEL_STA) {
                    wifi_scan();   /* 切回STA自动扫描 */
                }
            }
            s_wifi_sel = WIFI_ROW_MODE;
            wifi_rebuild_visible();
            return true;
        }
        /* 其他行：无动作 */
        return true;
    }

    if (key == KEY_A) {
        if (s_wifi_sel == WIFI_ROW_SWITCH) {
            /* WiFi开关 */
            if (st->wifi_on) {
                wifi_set_enabled(false);
            } else {
                wifi_set_enabled(true);
            }
            wifi_rebuild_visible();
            return true;
        }
        if (s_wifi_sel == WIFI_ROW_STATUS) {
            /* 状态行：手动扫描（STA）或AP详情提示 */
            if (st->wifi_on && s_mode_sel == WIFI_MODE_SEL_STA) {
                wifi_scan();
                wifi_rebuild_visible();
            }
            return true;
        }
        if (s_wifi_sel >= WIFI_ROW_NET_BASE && s_mode_sel == WIFI_MODE_SEL_STA) {
            /* 点击网络列表项：尝试连接/断开 */
            int net_idx = s_wifi_sel - WIFI_ROW_NET_BASE;
            if (net_idx < s_network_count) {
                if (s_connected_idx == net_idx) {
                    wifi_disconnect();
                } else {
                    wifi_connect(net_idx);
                }
                wifi_rebuild_visible();
            }
            return true;
        }
        return true;
    }

    return false;
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_wifi_settings_callbacks = {
    .init = wifi_settings_init,
    .destroy = wifi_settings_destroy,
    .on_key = wifi_settings_on_key,
};