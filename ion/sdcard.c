/**
 * @file ion/sdcard.c
 * @brief Ion - Hardware Abstraction Layer: SD Card Implementation
 *
 * SD 卡驱动实现。
 * 使用 ESP32-S3 SPI 模式驱动 SD 卡，通过 FATFS 文件系统挂载。
 * 支持挂载/卸载、空间查询、应用扫描。
 */

#include "ion/sdcard.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <string.h>
#include <sys/statvfs.h>

static const char *TAG = "ION_SD";

/* ========== 硬件配置 ========== */
#define SD_SPI_HOST        SPI2_HOST
#define SD_PIN_MOSI        GPIO_NUM_23
#define SD_PIN_MISO        GPIO_NUM_19
#define SD_PIN_SCLK        GPIO_NUM_18
#define SD_PIN_CS          GPIO_NUM_22

/* 注意：SD 卡与 LCD 共享 SPI2 总线，需要分时复用
 * LCD 使用 SPI2（CS=GPIO5，DC=GPIO4），SD 卡使用 SPI2（CS=GPIO22）
 * 两者不能同时使用，需确保驱动互斥 */

/* ========== 内部状态 ========== */
static bool s_mounted = false;
static char s_mount_point[32] = {0};
static sdmmc_card_t *s_card = NULL;

/* ========== 初始化 ========== */

bool ion_sdcard_init(const char *mount_point)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted at %s", s_mount_point);
        return true;
    }

    if (!mount_point) {
        mount_point = "/sdcard";
    }
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);

    ESP_LOGI(TAG, "Initializing SD card (SPI mode, mount=%s)", s_mount_point);

    /* 配置 SPI 总线
     * 注意：如果 LCD 已经初始化了 SPI2，这里需要额外的互斥处理
     * 当前方案：SD 卡和 LCD 共享 SPI2，但不同时使用 */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE 表示总线已初始化（被 LCD 使用），可以接受 */
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %d", ret);
        return false;
    }

    /* 配置 SD 卡 SPI 接口 */
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_PIN_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    /* 配置 FATFS 挂载选项 */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    /* 挂载 SD 卡 */
    ret = esp_vfs_fat_sdspi_mount(s_mount_point, &bus_cfg, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "SD card not found (no card inserted)");
        } else {
            ESP_LOGE(TAG, "Failed to mount SD card: %d", ret);
        }
        return false;
    }

    /* 打印卡信息 */
    sdmmc_card_print_info(stdout, s_card);

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", s_mount_point);
    return true;
}

/* ========== 挂载状态 ========== */

bool ion_sdcard_is_mounted(void)
{
    return s_mounted;
}

/* ========== 空间查询 ========== */

uint64_t ion_sdcard_get_free_space(void)
{
    if (!s_mounted) return 0;

    struct statvfs st;
    if (statvfs(s_mount_point, &st) != 0) {
        ESP_LOGE(TAG, "Failed to get filesystem stats");
        return 0;
    }

    uint64_t free_bytes = (uint64_t)st.f_bfree * st.f_bsize;
    return free_bytes;
}

uint64_t ion_sdcard_get_total_space(void)
{
    if (!s_mounted || !s_card) return 0;

    uint64_t total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    return total_bytes;
}

/* ========== 卸载 ========== */

bool ion_sdcard_unmount(void)
{
    if (!s_mounted) return true;

    ESP_LOGI(TAG, "Unmounting SD card from %s", s_mount_point);

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %d", ret);
        return false;
    }

    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
    return true;
}