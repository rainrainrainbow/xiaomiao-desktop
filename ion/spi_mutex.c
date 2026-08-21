/**
 * @file ion/spi_mutex.c
 * @brief Ion - Shared SPI Bus Mutex Implementation
 *
 * 使用 FreeRTOS 互斥锁（Mutex）实现 SPI 总线分时复用。
 * LCD 和 SD 卡共享 SPI2 总线，在访问 SPI 前需获取锁，完成后释放。
 */

#include "ion/spi_mutex.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "ION_SPI_MUTEX";

/* ========== 互斥锁句柄 ========== */
static SemaphoreHandle_t s_spi_mutex = NULL;

/* ========== 初始化 ========== */

void ion_spi_mutex_init(void)
{
    if (s_spi_mutex != NULL) {
        return;  /* 已初始化 */
    }

    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SPI mutex!");
        return;
    }

    ESP_LOGI(TAG, "SPI bus mutex initialized");
}

/* ========== 获取锁 ========== */

bool ion_spi_mutex_lock(uint32_t timeout_ms)
{
    if (s_spi_mutex == NULL) {
        ESP_LOGW(TAG, "SPI mutex not initialized, attempting lock anyway");
        return true;  /* 未初始化时放行（避免阻塞系统） */
    }

    TickType_t timeout_ticks;
    if (timeout_ms == 0) {
        timeout_ticks = portMAX_DELAY;  /* 无限等待 */
    } else {
        timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    }

    BaseType_t ret = xSemaphoreTake(s_spi_mutex, timeout_ticks);
    if (ret != pdTRUE) {
        ESP_LOGW(TAG, "SPI mutex lock timeout (%lu ms)", (unsigned long)timeout_ms);
        return false;
    }

    return true;
}

/* ========== 释放锁 ========== */

void ion_spi_mutex_unlock(void)
{
    if (s_spi_mutex == NULL) {
        return;
    }

    xSemaphoreGive(s_spi_mutex);
}

/* ========== 检查状态 ========== */

bool ion_spi_mutex_is_locked(void)
{
    if (s_spi_mutex == NULL) {
        return false;
    }

    /* 尝试获取锁，如果成功说明未被占用 */
    if (xSemaphoreTake(s_spi_mutex, 0) == pdTRUE) {
        xSemaphoreGive(s_spi_mutex);  /* 立即释放 */
        return false;
    }

    return true;
}