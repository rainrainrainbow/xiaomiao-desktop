/**
 * @file ion/spi_mutex.h
 * @brief Ion - Shared SPI Bus Mutex for LCD & SD Card
 *
 * ST7735 LCD 和 SD 卡共享 SPI2 总线，需要通过互斥锁确保分时复用。
 * 此模块提供统一的 SPI 总线互斥访问接口。
 */

#ifndef SPI_MUTEX_H
#define SPI_MUTEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SPI 总线互斥锁
 * @note 应在系统启动早期调用，在任何 SPI 操作之前
 */
void ion_spi_mutex_init(void);

/**
 * @brief 获取 SPI 总线锁（阻塞）
 * @param timeout_ms 超时时间（毫秒），0 = 无限等待
 * @return true 获取成功，false 超时
 */
bool ion_spi_mutex_lock(uint32_t timeout_ms);

/**
 * @brief 释放 SPI 总线锁
 */
void ion_spi_mutex_unlock(void);

/**
 * @brief 检查 SPI 总线是否被占用
 * @return true 已被占用
 */
bool ion_spi_mutex_is_locked(void);

#ifdef __cplusplus
}
#endif

#endif /* SPI_MUTEX_H */