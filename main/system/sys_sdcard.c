/**
 * @file sys_sdcard.c
 * @brief SD 卡文件系统管理实现
 */

#include "sys_sdcard.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "SYS_SD";
static bool s_sdcard_mounted = false;

/* ========== SD 卡初始化 ========== */
int sys_sdcard_init(void)
{
    if (s_sdcard_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return 0;
    }

    ESP_LOGI(TAG, "Initializing SD card...");

    // SD 卡配置（SPI 模式）
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST;  // 使用 HSPI
    
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;  // GPIO22 作为 SD 卡 CS
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(
        SDCARD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card
    );

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }

    // 打印 SD 卡信息
    sdmmc_card_print_info(stdout, card);
    s_sdcard_mounted = true;
    
    ESP_LOGI(TAG, "SD card mounted at %s", SDCARD_MOUNT_POINT);
    return 0;
}

/* ========== SD 卡卸载 ========== */
void sys_sdcard_deinit(void)
{
    if (!s_sdcard_mounted) {
        return;
    }
    
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT);
    s_sdcard_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

/* ========== 检查挂载状态 ========== */
bool sys_sdcard_is_mounted(void)
{
    return s_sdcard_mounted;
}

/* ========== 解析 app.json 文件 ========== */
static int parse_app_json(const char *path, app_meta_t *meta)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    // 简单 JSON 解析（不使用第三方库，手动解析）
    char line[256];
    memset(meta, 0, sizeof(app_meta_t));
    
    while (fgets(line, sizeof(line), f)) {
        // 解析 "name": "xxx"
        if (strstr(line, "\"name\"")) {
            char *start = strchr(line, ':');
            if (start) {
                start++;
                while (*start == ' ' || *start == '"') start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 0 && len < sizeof(meta->name)) {
                        strncpy(meta->name, start, len);
                        meta->name[len] = '\0';
                    }
                }
            }
        }
        // 解析 "entry": "xxx"
        else if (strstr(line, "\"entry\"")) {
            char *start = strchr(line, ':');
            if (start) {
                start++;
                while (*start == ' ' || *start == '"') start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 0 && len < sizeof(meta->entry)) {
                        strncpy(meta->entry, start, len);
                        meta->entry[len] = '\0';
                    }
                }
            }
        }
        // 解析 "icon": "xxx"
        else if (strstr(line, "\"icon\"")) {
            char *start = strchr(line, ':');
            if (start) {
                start++;
                while (*start == ' ' || *start == '"') start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 0 && len < sizeof(meta->icon)) {
                        strncpy(meta->icon, start, len);
                        meta->icon[len] = '\0';
                    }
                }
            }
        }
        // 解析 "description": "xxx"
        else if (strstr(line, "\"description\"")) {
            char *start = strchr(line, ':');
            if (start) {
                start++;
                while (*start == ' ' || *start == '"') start++;
                char *end = strchr(start, '"');
                if (end) {
                    int len = end - start;
                    if (len > 0 && len < sizeof(meta->description)) {
                        strncpy(meta->description, start, len);
                        meta->description[len] = '\0';
                    }
                }
            }
        }
    }

    fclose(f);
    
    // 验证必要字段
    if (strlen(meta->name) == 0 || strlen(meta->entry) == 0) {
        ESP_LOGW(TAG, "Invalid app.json: missing name or entry");
        return -1;
    }
    
    return 0;
}

/* ========== 扫描 SD 卡应用 ========== */
int sys_sdcard_scan_apps(app_meta_t *apps, int max_count)
{
    if (!s_sdcard_mounted) {
        ESP_LOGW(TAG, "SD card not mounted");
        return 0;
    }

    DIR *dir = opendir(SDCARD_APPS_PATH);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s directory", SDCARD_APPS_PATH);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        // 跳过 . 和 ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 构造 app.json 路径
        char json_path[128];
        snprintf(json_path, sizeof(json_path), "%s/%s/app.json", SDCARD_APPS_PATH, entry->d_name);
        
        // 检查 app.json 是否存在
        FILE *f = fopen(json_path, "r");
        if (!f) {
            continue;  // 不是有效的应用目录
        }
        fclose(f);
        
        // 解析 app.json
        app_meta_t meta;
        if (parse_app_json(json_path, &meta) == 0) {
            // 构造完整的入口文件路径
            char full_entry[128];
            snprintf(full_entry, sizeof(full_entry), "%s/%s/%s", SDCARD_APPS_PATH, entry->d_name, meta.entry);
            
            // 检查入口文件是否存在
            f = fopen(full_entry, "r");
            if (f) {
                fclose(f);
                
                // 复制元数据
                apps[count] = meta;
                strncpy(apps[count].entry, full_entry, sizeof(apps[count].entry) - 1);
                apps[count].entry[sizeof(apps[count].entry) - 1] = '\0';
                
                ESP_LOGI(TAG, "Found app: %s (%s)", meta.name, full_entry);
                count++;
            } else {
                ESP_LOGW(TAG, "Entry file not found: %s", full_entry);
            }
        }
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "Scanned %d apps from SD card", count);
    return count;
}