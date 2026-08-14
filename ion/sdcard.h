/**
 * @file ion/sdcard.h
 * @brief Ion - Hardware Abstraction Layer: SD Card Interface
 * 
 * 参考 NumWorks Epsilon 的 Ion 层设计，提供统一的 SD 卡管理接口。
 */

#ifndef ION_SDCARD_H
#define ION_SDCARD_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化 SD 卡驱动
 * @param mount_point 挂载点路径（如 "/sdcard"）
 * @return true 成功，false 失败
 */
bool ion_sdcard_init(const char *mount_point);

/**
 * @brief 检查 SD 卡是否已挂载
 * @return true 已挂载，false 未挂载
 */
bool ion_sdcard_is_mounted(void);

/**
 * @brief 获取 SD 卡可用空间（字节）
 * @return 可用空间字节数，0 表示未挂载或错误
 */
uint64_t ion_sdcard_get_free_space(void);

/**
 * @brief 获取 SD 卡总容量（字节）
 * @return 总容量字节数，0 表示未挂载或错误
 */
uint64_t ion_sdcard_get_total_space(void);

/**
 * @brief 卸载 SD 卡
 * @return true 成功，false 失败
 */
bool ion_sdcard_unmount(void);

#endif /* ION_SDCARD_H */