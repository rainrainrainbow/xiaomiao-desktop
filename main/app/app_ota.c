/**
 * @file app_ota.c
 * @brief OTA更新应用 - A/B 安全升级 + 更新日志 + 版本对比
 *
 * 安全策略：
 *   1. 固件写入 esp_ota_get_next_update_partition() 返回的"备用槽"(ota_1)，
 *      不再原地覆盖当前运行的 ota_0；下载/校验失败时当前固件完好，不变砖。
 *   2. 升级前把关键 NVS 配置导出到 /flash 作为备份。
 *   3. 校验通过后 set_boot_partition 切到新槽；READY 状态提供
 *      "重启生效 / 回滚上一版本"两个入口（LEFT/RIGHT 选择，A 确认）。
 *   4. release 的 body 解析为更新日志（静态缓冲，cJSON 用完即释放）。
 *   5. 本地版本与 tag_name 逐段数字比较；远端 <= 本地时不自动下载，
 *      但保留 A 键强制升级入口。
 */

#include "app_ota.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "app_sys_api.h"     /* app_sys_version / app_sys_cfg_get_int / app_sys_flash_free_kb */
#include "sys_nvs.h"         /* sys_nvs_load_wifi_credentials：关键配置备份 */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"      /* esp_restart */
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>          /* strtoul */

static const char *TAG = "APP_OTA";

/* GitHub API配置 */
#define GITHUB_API_URL "https://api.github.com/repos/rainrainrainbow/xiaomiao-desktop/releases/latest"
#define FIRMWARE_FILENAME "xiaomiao-desktop.bin"

/* 升级前配置备份文件（内部 FAT /flash） */
#define OTA_CFG_BACKUP_PATH "/flash/ota_cfg_backup.txt"

/* 更新日志小屏(160x128)渲染：每行截断字节数 / 可见行数 */
#define OTA_CHANGELOG_LINE_CHARS 20
#define OTA_CHANGELOG_VIS_LINES  5

/* OTA状态 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,   /* 校验中（esp_ota_end） */
    OTA_STATE_BACKUP,      /* 备份配置 */
    OTA_STATE_READY,       /* 新固件已写入备用槽，等待重启 */
    OTA_STATE_LATEST,      /* 已是最新版本（A 键强制升级） */
    OTA_STATE_FLASHING,
    OTA_STATE_ROLLBACK,    /* 已回滚，即将重启 */
    OTA_STATE_ERROR
} ota_state_t;

typedef enum {
    OTA_VIEW_MAIN = 0,
    OTA_VIEW_CHANGELOG
} ota_view_t;

static lv_obj_t *s_ota_list = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_version_label = NULL;
static lv_obj_t *s_action_label = NULL;
static lv_obj_t *s_changelog_title = NULL;
static lv_obj_t *s_changelog_label = NULL;
static volatile ota_state_t s_ota_state = OTA_STATE_IDLE;
static ota_view_t s_ota_view = OTA_VIEW_MAIN;
static char s_latest_version[32] = {0};
static char s_download_url[256] = {0};
static char s_changelog[512] = {0};   /* 静态缓冲：release body，cJSON 用完即释放 */
static volatile int s_download_progress = 0;
static int s_changelog_scroll = 0;    /* 更新日志滚动起始行 */
static int s_action_sel = 0;          /* READY 时 0=重启生效 1=回滚上一版本 */
static bool s_backup_ok = false;      /* 配置备份是否成功 */

/* 前向声明 */
static void ota_refresh(void);
static void ota_timer_cb(lv_timer_t *timer);
static void ota_rollback_reboot_cb(lv_timer_t *timer);

/* ===== 本地双语提示：lang 中无对应条目，不改动 lang.c ===== */
static const char *ota_text_up_to_date(void)
{
    return (lang_get_current() == LANG_EN) ? "Already up to date" : "已是最新版本";
}

static const char *ota_text_rollback_hint(void)
{
    return (lang_get_current() == LANG_EN) ? "A=Rollback" : "A=回滚上一版本";
}

/* ===== 版本比较：去 v 前缀，按 '.' 分段逐段数字比较 =====
 * 返回 >0: local 更新；<0: remote 更新；0: 相同。
 * 非数字段(如 -beta)按字符序兜底，避免 cJSON/sscanf 常驻开销。 */
static int ota_version_compare(const char *local, const char *remote)
{
    if (!local)  local = "";
    if (!remote) remote = "";
    while (*local  == 'v' || *local  == 'V') local++;
    while (*remote == 'v' || *remote == 'V') remote++;

    while (*local && *remote) {
        char *le = NULL;
        char *re = NULL;
        unsigned long lv = strtoul(local, &le, 10);
        unsigned long rv = strtoul(remote, &re, 10);
        if (lv != rv) return (lv > rv) ? 1 : -1;
        /* 分段未解析出数字（如 "-rc1"），按字符序兜底 */
        if (le == local || re == remote) {
            if (*local != *remote) return (*local > *remote) ? 1 : -1;
            local++;
            remote++;
            continue;
        }
        if (*le  == '.') le++;
        if (*re  == '.') re++;
        local  = le;
        remote = re;
    }
    if (*local)  return 1;   /* remote 为 local 的前缀，视为 local 更新 */
    if (*remote) return -1;
    return 0;
}

/* 更新日志总行数（用于滚动边界） */
static int ota_changelog_line_count(void)
{
    int n = 0;
    if (s_changelog[0] == '\0') return 0;
    for (const char *p = s_changelog; *p; p++) {
        if (*p == '\n') n++;
    }
    if (s_changelog[strlen(s_changelog) - 1] != '\n') n++;
    return n;
}

/* 把 s_changelog 从第 scroll 行起渲染到 dst（每行截断，适配小屏） */
static void ota_changelog_render(char *dst, size_t dst_size, int scroll)
{
    const char *p = s_changelog;
    int line = 0;
    size_t used = 0;
    dst[0] = '\0';

    if (s_changelog[0] == '\0') {
        snprintf(dst, dst_size, "%s", lang_get(STR_COMMON_LOADING));
        return;
    }

    while (*p && used + 1 < dst_size) {
        const char *end = p;
        while (*end && *end != '\n') end++;
        if (line >= scroll) {
            int row = line - scroll;
            if (row >= OTA_CHANGELOG_VIS_LINES) break;
            size_t len = (size_t)(end - p);
            if (len > OTA_CHANGELOG_LINE_CHARS) len = OTA_CHANGELOG_LINE_CHARS;
            while (len > 0 && p[len - 1] == '\r') len--;  /* 去 \r */
            /* 不在 UTF-8 多字节字符中间截断，避免行尾乱码 */
            while (len > 0 && ((unsigned char)p[len] & 0xC0) == 0x80) len--;
            size_t can = dst_size - used - 1;
            if (len > can) len = can;
            memcpy(dst + used, p, len);
            used += len;
            dst[used] = '\0';
            if (used + 1 < dst_size && *end) {
                dst[used++] = '\n';
                dst[used] = '\0';
            }
        }
        line++;
        if (!*end) break;
        p = end + 1;
    }
    if (used == 0) snprintf(dst, dst_size, "--");
}

/* ===== 升级前备份关键 NVS 配置到 /flash（简单文本导出）=====
 * 新固件首次启动可据此恢复；本页不提供恢复入口，避免误操作。
 * 失败不阻断升级，仅记录日志并由状态标签提示。 */
static bool ota_backup_config(void)
{
    uint32_t free_kb = app_sys_flash_free_kb();
    if (free_kb < 8) {
        ESP_LOGE(TAG, "Backup skipped: /flash free only %lu KB", (unsigned long)free_kb);
        return false;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    bool wifi_ok = sys_nvs_load_wifi_credentials(ssid, pass);

    FILE *fp = fopen(OTA_CFG_BACKUP_PATH, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Backup failed: cannot open %s", OTA_CFG_BACKUP_PATH);
        return false;
    }

    fprintf(fp, "# xiaomiao-desktop OTA config backup\n");
    fprintf(fp, "brightness=%d\n",   app_sys_cfg_get_int(APP_CFG_BRIGHTNESS, 60));
    fprintf(fp, "volume=%d\n",       app_sys_cfg_get_int(APP_CFG_VOLUME, 50));
    fprintf(fp, "sound=%d\n",        app_sys_cfg_get_int(APP_CFG_SOUND_ON, 1));
    fprintf(fp, "theme=%d\n",        app_sys_cfg_get_int(APP_CFG_THEME, 0));
    fprintf(fp, "wifi_on=%d\n",      app_sys_cfg_get_int(APP_CFG_WIFI_ON, 1));
    fprintf(fp, "layout=%d\n",       app_sys_cfg_get_int(APP_CFG_LAYOUT, 0));
    fprintf(fp, "font_size=%d\n",    app_sys_cfg_get_int(APP_CFG_FONT_SIZE, 14));
    fprintf(fp, "sleep=%d\n",        app_sys_cfg_get_int(APP_CFG_SLEEP_TIMEOUT, 30));
    fprintf(fp, "language=%d\n",     app_sys_cfg_get_int(APP_CFG_LANGUAGE, 0));
    fprintf(fp, "font_src=%d\n",     app_sys_cfg_get_int(APP_CFG_FONT_SOURCE, 0));
    fprintf(fp, "audio_out=%d\n",    app_sys_cfg_get_int(APP_CFG_AUDIO_OUT, 0));
    fprintf(fp, "music_eq=%d\n",     app_sys_cfg_get_int(APP_CFG_MUSIC_EQ, 0));
    fprintf(fp, "music_lrc=%d\n",    app_sys_cfg_get_int(APP_CFG_MUSIC_LRC, 0));
    fprintf(fp, "music_mode=%d\n",   app_sys_cfg_get_int(APP_CFG_MUSIC_MODE, 0));
    fprintf(fp, "led_vis_mode=%d\n", app_sys_cfg_get_int(APP_CFG_LED_VIS_MODE, 0));
    fprintf(fp, "led_vis_color=%d\n",app_sys_cfg_get_int(APP_CFG_LED_VIS_COLOR, 4));
    fprintf(fp, "wifi_ssid=%s\n",    wifi_ok ? ssid : "");
    fprintf(fp, "wifi_pass=%s\n",    wifi_ok ? pass : "");
    fclose(fp);

    ESP_LOGI(TAG, "Config backup saved: %s (wifi=%d)", OTA_CFG_BACKUP_PATH, wifi_ok);
    return true;
}

/* HTTP事件处理 */
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        /* 数据块日志改为 DEBUG 级别，避免固件下载时刷屏 */
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

/* 检查GitHub最新版本：解析 tag_name + assets + body(更新日志) + 版本对比 */
static void ota_check_latest_version(void)
{
    ESP_LOGI(TAG, "Checking latest version from GitHub...");

    /* 静态分配：原先在 ota_task 栈上，叠加 HTTPS/TLS 握手数 KB 开销会栈溢出。
     * 增大到 8KB：GitHub latest release JSON 含 release notes(markdown)，
     * 常超过 4KB，截断会导致 cJSON_Parse 失败 → OTA 检查假失败。 */
    static char response_buffer[8192];
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 10000,          /* 版本检查请求超时10秒，避免无网络时卡死 */
        .buffer_size = 4096,
        .skip_cert_common_name_check = true,  /* 设备时间未同步时放宽证书 CN 校验 */
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_cleanup(client);
        return;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_cleanup(client);
        return;
    }

    /* 读取响应内容（限制最大读取 sizeof(response_buffer)-1 字节） */
    int read_len = esp_http_client_read_response(client, response_buffer, sizeof(response_buffer) - 1);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Failed to read response");
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_cleanup(client);
        return;
    }
    response_buffer[read_len] = '\0';  /* 确保字符串终止 */

    /* 解析JSON */
    cJSON *root = cJSON_Parse(response_buffer);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_cleanup(client);
        return;
    }

    bool has_asset = false;
    cJSON *tag_name = cJSON_GetObjectItem(root, "tag_name");
    if (cJSON_IsString(tag_name)) {
        strncpy(s_latest_version, tag_name->valuestring, sizeof(s_latest_version) - 1);
        ESP_LOGI(TAG, "Latest version: %s", s_latest_version);

        /* 更新日志：release body 拷入静态缓冲，cJSON 随后立即释放 */
        cJSON *body = cJSON_GetObjectItem(root, "body");
        if (cJSON_IsString(body) && body->valuestring) {
            strncpy(s_changelog, body->valuestring, sizeof(s_changelog) - 1);
            s_changelog[sizeof(s_changelog) - 1] = '\0';
            ESP_LOGI(TAG, "Changelog parsed: %d bytes", (int)strlen(s_changelog));
        }
        s_changelog_scroll = 0;

        /* 查找固件下载链接 */
        cJSON *assets = cJSON_GetObjectItem(root, "assets");
        if (cJSON_IsArray(assets)) {
            cJSON *asset = NULL;
            cJSON_ArrayForEach(asset, assets) {
                cJSON *name = cJSON_GetObjectItem(asset, "name");
                if (cJSON_IsString(name) && strcmp(name->valuestring, FIRMWARE_FILENAME) == 0) {
                    cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
                    if (cJSON_IsString(url)) {
                        strncpy(s_download_url, url->valuestring, sizeof(s_download_url) - 1);
                        ESP_LOGI(TAG, "Download URL: %s", s_download_url);
                        has_asset = true;
                    }
                    break;
                }
            }
        }
    }
    cJSON_Delete(root);   /* 立即释放，更新日志已拷入 s_changelog */
    esp_http_client_cleanup(client);

    /* 兜底：未找到可下载的固件资产 → 错误，提示用户重试 */
    if (!has_asset) {
        ESP_LOGW(TAG, "No firmware asset '%s' found in latest release", FIRMWARE_FILENAME);
        s_latest_version[0] = '\0';
        s_download_url[0] = '\0';
        s_changelog[0] = '\0';
        s_download_progress = 0;
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* 版本对比：远端 <= 本地则不自动下载，但仍保留强制升级入口 */
    int cmp = ota_version_compare(app_sys_version(), s_latest_version);
    ESP_LOGI(TAG, "Version compare local(%s) vs remote(%s) = %d",
             app_sys_version(), s_latest_version, cmp);
    s_ota_state = (cmp >= 0) ? OTA_STATE_LATEST : OTA_STATE_DOWNLOADING;
}

/* ========== 流式 OTA：边下载边写入"备用槽"，不依赖 SD 卡 ==========
 * 旧实现原地覆盖当前运行的 launcher(ota_0)，无 A/B 备份，校验失败即变砖。
 * 现在通过 esp_ota_get_next_update_partition(NULL) 取备用分区（本设备为
 * ota_1/launcher_b），失败时当前 ota_0 固件完好。 */
#define OTA_HTTP_BUFFER_SIZE  2048
#define OTA_MIN_FIRMWARE_SIZE (64 * 1024)   /* 小于此值视为无效响应(如错误页) */

static void ota_stream_download_flash(void)
{
    ESP_LOGI(TAG, "Stream OTA from: %s", s_download_url);

    /* A/B 安全升级：写入备用槽而非当前运行槽（不硬编码地址/label） */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ESP_LOGE(TAG, "No spare OTA partition available for A/B update");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    if (running) {
        ESP_LOGI(TAG, "Running: %s (0x%lx); updating spare: %s (0x%lx, %lu bytes)",
                 running->label, (unsigned long)running->address,
                 partition->label, (unsigned long)partition->address,
                 (unsigned long)partition->size);
    }

    /* 升级前备份关键配置（失败不阻断升级，仅记录，READY 时提示） */
    s_ota_state = OTA_STATE_BACKUP;
    s_backup_ok = ota_backup_config();
    if (!s_backup_ok) {
        ESP_LOGW(TAG, "Config backup failed; upgrade continues (settings may be lost after switch)");
    }
    s_download_progress = 0;

    esp_http_client_config_t config = {
        .url = s_download_url,
        .event_handler = _http_event_handler,
        .timeout_ms = 30000,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = OTA_HTTP_BUFFER_SIZE,
        .skip_cert_common_name_check = true,  /* ESP32 时间未同步时放宽证书 CN 校验 */
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* fetch_headers 先读响应头，之后 get_status_code 才有有效值 */
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d content_length=%d", status_code, content_length);
    if (status_code != 200) {
        ESP_LOGE(TAG, "Download HTTP status %d != 200", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    /* Content-Length 必须合理：过大超分区、过小是错误页 */
    if (content_length > 0 &&
        ((size_t)content_length > partition->size || content_length < OTA_MIN_FIRMWARE_SIZE)) {
        ESP_LOGE(TAG, "Invalid content_length=%d (partition=%lu, min=%d)",
                 content_length, (unsigned long)partition->size, OTA_MIN_FIRMWARE_SIZE);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* 分配缓冲：优先内部 RAM（esp_ota_write 需要），不足则退回普通堆 */
    uint8_t *buf = heap_caps_malloc(OTA_HTTP_BUFFER_SIZE, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    esp_ota_handle_t update_handle = 0;
    err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        heap_caps_free(buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    s_ota_state = OTA_STATE_DOWNLOADING;

    int total_read = 0;
    int last_log_pct = -10;
    bool first_chunk = true;
    while (1) {
        int read_len = esp_http_client_read(client, (char *)buf, OTA_HTTP_BUFFER_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error at %d bytes", total_read);
            break;
        }
        if (read_len == 0) break;  /* 流结束 */

        /* 首块校验：ESP32 app image 必须以 0xE9 魔数开头 */
        if (first_chunk) {
            first_chunk = false;
            if ((uint8_t)buf[0] != 0xE9) {
                ESP_LOGE(TAG, "Invalid firmware magic 0x%02x (expected 0xE9)", buf[0]);
                break;
            }
        }

        err = esp_ota_write(update_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total_read += read_len;

        int pct = content_length > 0 ? (total_read * 100) / content_length
                                     : (total_read / 1024) % 100;
        if (pct > 100) pct = 100;
        s_download_progress = pct;
        if (pct >= last_log_pct + 10) {
            ESP_LOGI(TAG, "OTA progress: %d%% (%d KB)", pct, total_read / 1024);
            last_log_pct = pct;
        }
    }

    heap_caps_free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* 下载不完整 / 无数据：放弃（备用槽作废，当前运行槽不受影响） */
    if (total_read < OTA_MIN_FIRMWARE_SIZE ||
        (content_length > 0 && total_read != content_length)) {
        ESP_LOGE(TAG, "Download incomplete: got %d / %d bytes, abort OTA",
                 total_read, content_length);
        esp_ota_abort(update_handle);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* esp_ota_end 会校验镜像完整性(sha/长度) */
    s_ota_state = OTA_STATE_VERIFYING;
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (firmware image invalid)", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* 读取新固件版本描述，仅用于日志核对（不常驻大内存） */
    esp_app_desc_t new_desc;
    if (esp_ota_get_partition_description(partition, &new_desc) == ESP_OK) {
        ESP_LOGI(TAG, "New firmware version: %s (built %s)", new_desc.version, new_desc.date);
    }

    /* 回滚策略说明：
     * 这里不调用 esp_ota_mark_app_valid_cancel_rollback()——它标记的是"当前运行"
     * 的旧固件，对新槽无效。新固件应在自身启动后自行标记 valid（见交付说明）。
     * 若 sdkconfig 打开 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，bootloader 会在
     * 新固件连续未标记 valid 时自动回退到旧槽，形成第二道安全网。 */
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    ESP_LOGI(TAG, "OTA ready: %d bytes written to spare slot %s. Reboot to apply.",
             total_read, partition->label);
    s_download_progress = 100;
    s_action_sel = 0;
    s_ota_state = OTA_STATE_READY;
}

/* OTA任务：检查版本 + 流式下载刷写（一体化，不依赖 SD 卡） */
static void ota_task(void *pvParameters)
{
    ota_check_latest_version();

    if (s_ota_state == OTA_STATE_DOWNLOADING && strlen(s_download_url) > 0) {
        ota_stream_download_flash();
    }

    vTaskDelete(NULL);
}

/* 强制升级任务：跳过版本检查，直接下载备用槽（"已是最新"时 A 键入口） */
static void ota_task_force_download(void *pvParameters)
{
    ota_stream_download_flash();
    vTaskDelete(NULL);
}

/* 启动OTA任务 */
static void ota_start(void)
{
    /* 允许从 ERROR / LATEST 状态重试或重新检查 */
    if (s_ota_state != OTA_STATE_IDLE && s_ota_state != OTA_STATE_ERROR &&
        s_ota_state != OTA_STATE_LATEST) {
        ESP_LOGW(TAG, "OTA already in progress (state=%d)", s_ota_state);
        return;
    }

    s_ota_state = OTA_STATE_CHECKING;
    s_download_progress = 0;
    s_ota_view = OTA_VIEW_MAIN;
    s_changelog_scroll = 0;
    s_action_sel = 0;
    s_backup_ok = false;
    memset(s_latest_version, 0, sizeof(s_latest_version));
    memset(s_download_url, 0, sizeof(s_download_url));
    memset(s_changelog, 0, sizeof(s_changelog));

    /* 提到 12KB：即便 response_buffer 已静态分配，HTTPS/TLS 握手本身
     * 仍需数 KB 栈，给足余量避免与已修复的栈溢出同类问题。 */
    xTaskCreate(ota_task, "ota_task", 12288, NULL, 5, NULL);
}

/* 强制升级：复用已缓存的 s_download_url，直接进入下载任务 */
static void ota_force_download(void)
{
    if (strlen(s_download_url) == 0) {
        ESP_LOGW(TAG, "Force upgrade: no URL cached, re-checking");
        ota_start();
        return;
    }
    s_ota_state = OTA_STATE_DOWNLOADING;
    s_download_progress = 0;
    s_backup_ok = false;
    xTaskCreate(ota_task_force_download, "ota_force_dl", 12288, NULL, 5, NULL);
}

/* 回滚到上一版本：把启动分区切到"非当前启动"的另一个 OTA 槽并重启 */
static void ota_rollback_to_previous(void)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!boot) boot = esp_ota_get_running_partition();
    if (!boot) {
        ESP_LOGE(TAG, "Rollback failed: no boot/running partition");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    /* 两个 OTA 槽时，next(boot) 即另一个槽（旧固件所在槽） */
    const esp_partition_t *target = esp_ota_get_next_update_partition(boot);
    if (!target || target == boot) {
        ESP_LOGE(TAG, "Rollback failed: no alternate OTA partition");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }

    ESP_LOGI(TAG, "Rollback: boot %s -> %s", boot->label, target->label);
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback set_boot_partition failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    s_ota_state = OTA_STATE_ROLLBACK;
    /* 延时重启，让用户看到"已回滚"提示（一次性定时器） */
    lv_timer_t *t = lv_timer_create(ota_rollback_reboot_cb, 1200, NULL);
    if (t) lv_timer_set_repeat_count(t, 1);
}

/* 延时重启：READY 状态 A=重启生效 使用（一次性定时器） */
static void ota_delayed_reboot_cb(lv_timer_t *timer)
{
    /* repeat_count=1，LVGL 执行后自动删除；esp_restart 不会返回 */
    ESP_LOGI(TAG, "Rebooting to apply OTA...");
    esp_restart();
}

/* 刷新UI（仅改文本/样式，不创建/删除对象） */
static void ota_refresh(void)
{
    if (!s_ota_list) return;

    bool main_view = (s_ota_view == OTA_VIEW_MAIN);

#define OTA_VIS(obj, vis) do { if (obj) { \
        if (vis) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN); \
        else     lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); \
    } } while (0)

    OTA_VIS(s_status_label,   main_view);
    OTA_VIS(s_progress_bar,   main_view);
    OTA_VIS(s_version_label,  main_view);
    OTA_VIS(s_action_label,   main_view);
    OTA_VIS(s_changelog_title, !main_view);
    OTA_VIS(s_changelog_label, !main_view);

    /* 更新状态标签 */
    if (s_status_label) {
        char buf[96];
        const char *status_text = "";
        switch (s_ota_state) {
        case OTA_STATE_IDLE:
            status_text = lang_get(STR_OTA_IDLE);
            break;
        case OTA_STATE_CHECKING:
            status_text = lang_get(STR_OTA_CHECKING);
            break;
        case OTA_STATE_BACKUP:
            status_text = lang_get(STR_OTA_BACKUP);
            break;
        case OTA_STATE_DOWNLOADING:
            snprintf(buf, sizeof(buf), "%s (%s)",
                     lang_get(STR_OTA_DOWNLOADING), lang_get(STR_OTA_SAFEMODE));
            status_text = buf;
            break;
        case OTA_STATE_VERIFYING:
            status_text = lang_get(STR_OTA_VERIFYING);
            break;
        case OTA_STATE_READY:
            if (s_backup_ok) {
                status_text = lang_get(STR_OTA_READY);
            } else {
                snprintf(buf, sizeof(buf), "%s (%s:%s)", lang_get(STR_OTA_READY),
                         lang_get(STR_OTA_BACKUP), lang_get(STR_COMMON_FAILED));
                status_text = buf;
            }
            break;
        case OTA_STATE_LATEST:
            snprintf(buf, sizeof(buf), "%s (%s)",
                     ota_text_up_to_date(), lang_get(STR_OTA_SAFEMODE));
            status_text = buf;
            break;
        case OTA_STATE_FLASHING:
            status_text = lang_get(STR_OTA_DOWNLOADING);
            break;
        case OTA_STATE_ROLLBACK:
            status_text = lang_get(STR_OTA_ROLLBACK);
            break;
        case OTA_STATE_ERROR:
            status_text = lang_get(STR_OTA_ERROR);
            break;
        }
        lv_label_set_text(s_status_label, status_text);
    }

    /* 更新进度条 */
    if (s_progress_bar) {
        lv_bar_set_value(s_progress_bar, s_download_progress, LV_ANIM_ON);
    }

    /* 版本标签：顶部同时显示 当前版本 / 最新版本 */
    if (s_version_label) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s: %s\n%s: %s",
                 lang_get(STR_OTA_CURRENT), app_sys_version(),
                 lang_get(STR_OTA_LATEST),
                 strlen(s_latest_version) > 0 ? s_latest_version : "-");
        lv_label_set_text(s_version_label, buf);
    }

    /* 操作标签 */
    if (s_action_label) {
        char buf[96];
        if (s_ota_state == OTA_STATE_READY) {
            /* LEFT/RIGHT 选择：重启生效 / 回滚上一版本 */
            if (s_action_sel == 0) {
                snprintf(buf, sizeof(buf), "> %s | %s",
                         lang_get(STR_OTA_REBOOT_HINT), ota_text_rollback_hint());
            } else {
                snprintf(buf, sizeof(buf), "%s | > %s",
                         lang_get(STR_OTA_REBOOT_HINT), ota_text_rollback_hint());
            }
            lv_label_set_text(s_action_label, buf);
        } else if (s_ota_state == OTA_STATE_LATEST) {
            snprintf(buf, sizeof(buf), "A=%s (%s)",
                     lang_get(STR_COMMON_CONFIRM), lang_get(STR_OTA_SAFEMODE));
            lv_label_set_text(s_action_label, buf);
        } else if (s_ota_state == OTA_STATE_ERROR) {
            lv_label_set_text(s_action_label, lang_get(STR_OTA_RETRY_HINT));
        } else if (ota_changelog_line_count() > 0) {
            snprintf(buf, sizeof(buf), "UP: %s", lang_get(STR_OTA_CHANGELOG));
            lv_label_set_text(s_action_label, buf);
        } else {
            lv_label_set_text(s_action_label, "");
        }
    }

    /* 更新日志视图：按行渲染，适配小屏 */
    if (!main_view && s_changelog_label) {
        char buf[256];
        ota_changelog_render(buf, sizeof(buf), s_changelog_scroll);
        lv_label_set_text(s_changelog_label, buf);
    }
#undef OTA_VIS
}

/* 页面初始化 */
static lv_timer_t *s_ota_timer = NULL;

static void ota_init(void *data)
{
    ESP_LOGI(TAG, "OTA app init");

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);

    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);

    /* 创建列表容器 */
    s_ota_list = lv_list_create(scr);
    if (!s_ota_list) {
        ESP_LOGE(TAG, "lv_list_create(ota_list) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_set_size(s_ota_list, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_ota_list);

    /* 状态标签 */
    s_status_label = lv_label_create(s_ota_list);
    if (s_status_label) {
        lv_label_set_text(s_status_label, lang_get(STR_OTA_IDLE));
        lv_obj_set_width(s_status_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_status_label, lv_color_hex(colors->text), 0);
    }

    /* 进度条 */
    s_progress_bar = lv_bar_create(s_ota_list);
    if (s_progress_bar) {
        lv_obj_set_width(s_progress_bar, LV_PCT(80));
        lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 0);
        lv_bar_set_range(s_progress_bar, 0, 100);
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }

    /* 版本标签（当前 / 最新） */
    s_version_label = lv_label_create(s_ota_list);
    if (s_version_label) {
        lv_obj_set_width(s_version_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_version_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* 操作标签 */
    s_action_label = lv_label_create(s_ota_list);
    if (s_action_label) {
        lv_obj_set_width(s_action_label, LV_PCT(100));
        lv_obj_set_style_text_align(s_action_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_action_label, lv_color_hex(colors->text_dim), 0);
    }

    /* 更新日志标题（UP 进入，B/DOWN 返回） */
    s_changelog_title = lv_label_create(s_ota_list);
    if (s_changelog_title) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (UP/DOWN B)", lang_get(STR_OTA_CHANGELOG));
        lv_label_set_text(s_changelog_title, buf);
        lv_obj_set_width(s_changelog_title, LV_PCT(100));
        lv_obj_set_style_text_align(s_changelog_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_changelog_title, lv_color_hex(colors->text), 0);
        lv_obj_add_flag(s_changelog_title, LV_OBJ_FLAG_HIDDEN);
    }

    /* 更新日志正文：小屏按行渲染，左对齐自动换行 */
    s_changelog_label = lv_label_create(s_ota_list);
    if (s_changelog_label) {
        lv_obj_set_width(s_changelog_label, LV_PCT(96));
        lv_label_set_long_mode(s_changelog_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_changelog_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_color(s_changelog_label, lv_color_hex(colors->text), 0);
        lv_obj_add_flag(s_changelog_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* 创建定时器定期刷新UI（500ms间隔，更新进度条和状态） */
    s_ota_timer = lv_timer_create(ota_timer_cb, 500, NULL);

    /* 启动OTA检查 */
    ota_start();
}

/* 页面销毁 */
static void ota_destroy(void)
{
    ESP_LOGI(TAG, "OTA app destroy");
    if (s_ota_timer) {
        lv_timer_del(s_ota_timer);
        s_ota_timer = NULL;
    }
    s_ota_list = NULL;
    s_status_label = NULL;
    s_progress_bar = NULL;
    s_version_label = NULL;
    s_action_label = NULL;
    s_changelog_title = NULL;
    s_changelog_label = NULL;
}

/* 按键处理 */
static bool ota_on_key(int key)
{
    if (key == KEY_B) {
        /* 更新日志视图：B 先回主视图；主视图才退栈 */
        if (s_ota_view == OTA_VIEW_CHANGELOG) {
            s_ota_view = OTA_VIEW_MAIN;
            ota_refresh();
            return true;
        }
        ui_stack_pop();
        return true;
    }

    /* UP/DOWN：主视图→进日志；日志视图→按行滚动 */
    if (key == KEY_UP || key == KEY_DOWN) {
        if (s_ota_view == OTA_VIEW_MAIN) {
            s_ota_view = OTA_VIEW_CHANGELOG;
        } else {
            int max = ota_changelog_line_count() - OTA_CHANGELOG_VIS_LINES;
            if (max < 0) max = 0;
            if (key == KEY_DOWN) {
                s_changelog_scroll++;
                if (s_changelog_scroll > max) s_changelog_scroll = 0;
            } else {
                s_changelog_scroll--;
                if (s_changelog_scroll < 0) s_changelog_scroll = max;
            }
        }
        ota_refresh();
        return true;
    }

    /* LEFT/RIGHT：READY 时选择 重启/回滚；其余状态切换视图 */
    if (key == KEY_LEFT || key == KEY_RIGHT) {
        if (s_ota_view == OTA_VIEW_MAIN && s_ota_state == OTA_STATE_READY) {
            s_action_sel = (s_action_sel == 0) ? 1 : 0;
        } else {
            s_ota_view = (s_ota_view == OTA_VIEW_MAIN) ? OTA_VIEW_CHANGELOG : OTA_VIEW_MAIN;
        }
        ota_refresh();
        return true;
    }

    if (key == KEY_A) {
        if (s_ota_view == OTA_VIEW_CHANGELOG) {
            s_ota_view = OTA_VIEW_MAIN;
            ota_refresh();
            return true;
        }
        if (s_ota_state == OTA_STATE_IDLE || s_ota_state == OTA_STATE_ERROR) {
            ota_start();
        } else if (s_ota_state == OTA_STATE_LATEST) {
            /* 已是最新版本：A 键强制升级 */
            ota_force_download();
        } else if (s_ota_state == OTA_STATE_READY) {
            if (s_action_sel == 1) {
                /* 回滚入口：切回旧槽 */
                ota_rollback_to_previous();
            } else {
                /* 流式 OTA 已写入备用槽并设好启动分区，延时重启生效 */
                ESP_LOGI(TAG, "OTA firmware ready, rebooting to apply...");
                lv_timer_t *t = lv_timer_create(ota_delayed_reboot_cb, 800, NULL);
                if (t) lv_timer_set_repeat_count(t, 1);
            }
        }
        return true;
    }

    return false;
}

/* 定时器回调（500ms 周期刷新 UI；重启由独立的一次性定时器负责） */
static void ota_timer_cb(lv_timer_t *timer)
{
    ota_refresh();
}

/* 回滚后延时重启的独立回调（与 500ms 刷新定时器区分） */
static void ota_rollback_reboot_cb(lv_timer_t *timer)
{
    /* repeat_count=1，LVGL 执行后自动删除；esp_restart 不会返回 */
    ESP_LOGI(TAG, "Rolled back boot partition, rebooting...");
    esp_restart();
}

/* 页面回调 */
const page_callbacks_t g_ota_callbacks = {
    .init = ota_init,
    .destroy = ota_destroy,
    .on_key = ota_on_key,
};
