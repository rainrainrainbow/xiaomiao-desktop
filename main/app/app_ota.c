/**
 * @file app_ota.c
 * @brief OTA更新应用 - 从GitHub下载最新固件
 */

#include "app_ota.h"
#include "ui_framework.h"
#include "lang/lang.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static const char *TAG = "APP_OTA";

/* GitHub API配置 */
#define GITHUB_API_URL "https://api.github.com/repos/rainrainrainbow/xiaomiao-desktop/releases/latest"
#define FIRMWARE_FILENAME "xiaomiao-desktop.bin"
#define FIRMWARE_PATH "/sdcard/ota/" FIRMWARE_FILENAME

/* OTA状态 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_READY,
    OTA_STATE_FLASHING,
    OTA_STATE_ERROR
} ota_state_t;

static lv_obj_t *s_ota_list = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_version_label = NULL;
static lv_obj_t *s_action_label = NULL;
static volatile ota_state_t s_ota_state = OTA_STATE_IDLE;
static char s_latest_version[32] = {0};
static char s_download_url[256] = {0};
static volatile int s_download_progress = 0;

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

/* 检查GitHub最新版本 */
static void ota_check_latest_version(void)
{
    ESP_LOGI(TAG, "Checking latest version from GitHub...");
    
    char response_buffer[4096] = {0};
    
    esp_http_client_config_t config = {
        .url = GITHUB_API_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 10000,          /* 版本检查请求超时10秒，避免无网络时卡死 */
        .buffer_size = 4096,
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
    
    cJSON *tag_name = cJSON_GetObjectItem(root, "tag_name");
    if (cJSON_IsString(tag_name)) {
        strncpy(s_latest_version, tag_name->valuestring, sizeof(s_latest_version) - 1);
        ESP_LOGI(TAG, "Latest version: %s", s_latest_version);
        
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
                        s_ota_state = OTA_STATE_DOWNLOADING;
                    }
                    break;
                }
            }
        }
    }
    cJSON_Delete(root);
    
    esp_http_client_cleanup(client);

    /* 兜底：如果解析后仍未找到可下载的固件资产，进入错误状态以便用户重试 */
    if (s_ota_state == OTA_STATE_CHECKING) {
        ESP_LOGW(TAG, "No firmware asset '%s' found in latest release", FIRMWARE_FILENAME);
        s_latest_version[0] = '\0';
        s_download_url[0] = '\0';
        s_download_progress = 0;
        s_ota_state = OTA_STATE_ERROR;
    }
}
/* 下载固件到SD卡 */
static void ota_download_firmware(void)
{
    ESP_LOGI(TAG, "Downloading firmware to %s", FIRMWARE_PATH);
    
    /* 创建目录 */
    mkdir("/sdcard/ota", 0755);
    
    FILE *fp = fopen(FIRMWARE_PATH, "wb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    esp_http_client_config_t config = {
        .url = s_download_url,
        .event_handler = _http_event_handler,
        .timeout_ms = 30000,          /* 固件下载超时30秒 */
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        fclose(fp);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content length: %d", content_length);
    
    char *buffer = heap_caps_malloc(1024, MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        fclose(fp);
        esp_http_client_cleanup(client);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    int total_read = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buffer, 1024);
        if (read_len <= 0) {
            break;
        }
        
        fwrite(buffer, 1, read_len, fp);
        total_read += read_len;
        
        if (content_length > 0) {
            s_download_progress = (total_read * 100) / content_length;
            if (s_download_progress > 100) s_download_progress = 100;
        } else {
            /* 未知总大小，显示已下载字节数（取模100显示） */
            s_download_progress = (total_read / 1024) % 100;
        }
        /* 每10%打印一次日志，避免刷屏 */
        static int last_log_progress = -1;
        if (s_download_progress != last_log_progress) {
            ESP_LOGI(TAG, "Download progress: %d%% (%d KB)", s_download_progress, total_read / 1024);
            last_log_progress = s_download_progress;
        }
    }
    
    heap_caps_free(buffer);
    fclose(fp);
    esp_http_client_cleanup(client);
    
    if (total_read > 0 && (content_length <= 0 || total_read == content_length)) {
        ESP_LOGI(TAG, "Download complete: %d bytes", total_read);
        s_ota_state = OTA_STATE_READY;
    } else if (total_read > 0) {
        /* 下载不完整：实际字节数与 Content-Length 不符，视为失败 */
        ESP_LOGE(TAG, "Download incomplete: got %d / %d bytes, discarding",
                 total_read, content_length);
        remove(FIRMWARE_PATH);   /* 清理不完整的固件文件 */
        s_ota_state = OTA_STATE_ERROR;
    }
}

/* 从SD卡刷写固件到OTA分区 */
static void ota_flash_from_sdcard(void)
{
    ESP_LOGI(TAG, "Flashing firmware from SD card...");
    
    FILE *fp = fopen(FIRMWARE_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open firmware file: %s", FIRMWARE_PATH);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    /* 获取文件大小 */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid firmware file size: %ld", file_size);
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    ESP_LOGI(TAG, "Firmware size: %ld bytes", file_size);
    
    /* 获取launcher(ota_0)分区 */
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0,
        "launcher"
    );
    
    if (partition == NULL) {
        ESP_LOGE(TAG, "Failed to find launcher partition");
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    ESP_LOGI(TAG, "Target partition: %s (offset=0x%lx, size=%lu)", 
             partition->label, (unsigned long)partition->address, (unsigned long)partition->size);
    
    /* 验证固件大小是否适合分区 */
    if ((size_t)file_size > partition->size) {
        ESP_LOGE(TAG, "Firmware too large for partition: %ld > %lu", file_size, (unsigned long)partition->size);
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    /* 开始OTA更新 */
    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(partition, file_size, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    /* 分配读取缓冲区 */
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate flash buffer");
        esp_ota_abort(update_handle);
        fclose(fp);
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    /* 分块读取并写入 */
    int total_written = 0;
    while (1) {
        size_t read_len = fread(buf, 1, 4096, fp);
        if (read_len == 0) break;
        
        err = esp_ota_write(update_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            heap_caps_free(buf);
            esp_ota_abort(update_handle);
            fclose(fp);
            s_ota_state = OTA_STATE_ERROR;
            return;
        }
        
        total_written += read_len;
        
        /* 更新进度 */
        if (file_size > 0) {
            s_download_progress = (total_written * 100) / file_size;
            if (s_download_progress > 100) s_download_progress = 100;
        }
        
        /* 每10%打印一次日志 */
        static int last_log_progress = -1;
        if (s_download_progress >= last_log_progress + 10) {
            ESP_LOGI(TAG, "Flash progress: %d%% (%d/%ld KB)", 
                     s_download_progress, total_written / 1024, file_size / 1024);
            last_log_progress = s_download_progress;
        }
    }
    
    heap_caps_free(buf);
    fclose(fp);
    
    /* 完成OTA更新 */
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    /* 设置启动分区 */
    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        return;
    }
    
    ESP_LOGI(TAG, "OTA flash complete! New boot partition: %s", partition->label);
    ESP_LOGI(TAG, "Rebooting in 2 seconds...");
    
    /* 延迟2秒后重启 */
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* OTA任务 */
static void ota_task(void *pvParameters)
{
    /* 检查最新版本 */
    ota_check_latest_version();
    
    if (s_ota_state == OTA_STATE_DOWNLOADING && strlen(s_download_url) > 0) {
        /* 下载固件 */
        ota_download_firmware();
    }
    
    vTaskDelete(NULL);
}

/* 刷写任务：从SD卡刷写固件（独立任务，避免阻塞LVGL主线程导致UI冻结） */
static void ota_flash_task(void *pvParameters)
{
    (void)pvParameters;
    s_ota_state = OTA_STATE_FLASHING;
    s_download_progress = 0;
    ota_flash_from_sdcard();
    vTaskDelete(NULL);
}

/* 启动OTA任务 */
static void ota_start(void)
{
    if (s_ota_state != OTA_STATE_IDLE) {
        ESP_LOGW(TAG, "OTA already in progress");
        return;
    }
    
    s_ota_state = OTA_STATE_CHECKING;
    s_download_progress = 0;
    memset(s_latest_version, 0, sizeof(s_latest_version));
    memset(s_download_url, 0, sizeof(s_download_url));
    
    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
}

/* 前向声明 */
static void ota_timer_cb(lv_timer_t *timer);

/* 刷新UI */
static void ota_refresh(void)
{
    if (!s_ota_list) return;
    
    const theme_colors_t *colors = ui_theme_colors();
    
    /* 更新状态标签 */
    if (s_status_label) {
        const char *status_text = "";
        switch (s_ota_state) {
        case OTA_STATE_IDLE:
            status_text = lang_get(STR_OTA_IDLE);
            break;
        case OTA_STATE_CHECKING:
            status_text = lang_get(STR_OTA_CHECKING);
            break;
        case OTA_STATE_DOWNLOADING:
            status_text = lang_get(STR_OTA_DOWNLOADING);
            break;
        case OTA_STATE_READY:
            status_text = lang_get(STR_OTA_READY);
            break;
        case OTA_STATE_FLASHING:
            status_text = "Flashing...";
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
    
    /* 更新版本标签 */
    if (s_version_label) {
        if (strlen(s_latest_version) > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s: %s", lang_get(STR_OTA_LATEST_VERSION), s_latest_version);
            lv_label_set_text(s_version_label, buf);
        } else {
            lv_label_set_text(s_version_label, "");
        }
    }
    
    /* 更新操作标签 */
    if (s_action_label) {
        if (s_ota_state == OTA_STATE_READY) {
            lv_label_set_text(s_action_label, lang_get(STR_OTA_REBOOT_HINT));
        } else if (s_ota_state == OTA_STATE_ERROR) {
            lv_label_set_text(s_action_label, lang_get(STR_OTA_RETRY_HINT));
        } else {
            lv_label_set_text(s_action_label, "");
        }
    }
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
    }
    
    /* 进度条 */
    s_progress_bar = lv_bar_create(s_ota_list);
    if (s_progress_bar) {
        lv_obj_set_width(s_progress_bar, LV_PCT(80));
        lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 0);
        lv_bar_set_range(s_progress_bar, 0, 100);
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }
    
    /* 版本标签 */
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
    
    /* 创建定时器定期刷新UI（500ms间隔，更新进度条和状态） */
    s_ota_timer = lv_timer_create(ota_timer_cb, 500, NULL);
    
    /* 启动OTA检查 */
    ota_start();
}

/* 页面销毁 */
static void ota_destroy(void)
{
    ESP_LOGI(TAG, "OTA app destroy");
    /* 销毁定时器 */
    if (s_ota_timer) {
        lv_timer_del(s_ota_timer);
        s_ota_timer = NULL;
    }
    s_ota_list = NULL;
    s_status_label = NULL;
    s_progress_bar = NULL;
    s_version_label = NULL;
    s_action_label = NULL;
}

/* 按键处理 */
static bool ota_on_key(int key)
{
    if (key == KEY_B) {
        ui_stack_pop();
        return true;
    }
    
    if (key == KEY_A) {
        if (s_ota_state == OTA_STATE_IDLE || s_ota_state == OTA_STATE_ERROR) {
            /* 重新开始OTA */
            ota_start();
        } else if (s_ota_state == OTA_STATE_READY) {
            /* 开始刷写固件：放入独立任务，避免阻塞 LVGL 主线程 */
            ESP_LOGI(TAG, "Starting firmware flash from SD card...");
            s_ota_state = OTA_STATE_FLASHING;
            s_download_progress = 0;
            BaseType_t rt = xTaskCreate(ota_flash_task, "ota_flash", 8192, NULL, 5, NULL);
            if (rt != pdPASS) {
                ESP_LOGE(TAG, "xTaskCreate(ota_flash) failed! mem free=%lu",
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                s_ota_state = OTA_STATE_ERROR;
            }
        }
        return true;
    }
    
    return false;
}

/* 定时器回调（前向声明已在 ota_init 前使用，需在此定义） */
static void ota_timer_cb(lv_timer_t *timer)
{
    ota_refresh();
}

/* 页面回调 */
const page_callbacks_t g_ota_callbacks = {
    .init = ota_init,
    .destroy = ota_destroy,
    .on_key = ota_on_key,
};