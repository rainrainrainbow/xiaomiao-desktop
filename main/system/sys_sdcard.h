/**
 * @file sys_sdcard.h
 * @brief SD 卡文件系统管理 - FATFS 挂载和扫描
 */

#ifndef SYS_SDCARD_H
#define SYS_SDCARD_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

/* ========== SD 卡挂载配置 ========== */
#define SDCARD_MOUNT_POINT      "/sdcard"
#define SDCARD_APPS_PATH        "/sdcard/apps"
#define SDCARD_MAX_APPS         16

/* SD 卡 CS 引脚（SPI 模式） */
#define PIN_SD_CS               GPIO_NUM_22

/* ========== 应用元数据结构 ========== */
typedef struct {
    char name[32];          // 应用名称（中文）
    char entry[64];         // Python 入口文件路径
    char icon[8];           // 图标 emoji/字符
    char description[64];   // 应用描述
} app_meta_t;

/* ========== SD 卡管理接口 ========== */

/**
 * 初始化 SD 卡文件系统
 * @return 0 成功，其他失败
 */
int sys_sdcard_init(void);

/**
 * 卸载 SD 卡文件系统
 */
void sys_sdcard_deinit(void);

/**
 * 检查 SD 卡是否已挂载
 * @return true 已挂载
 */
bool sys_sdcard_is_mounted(void);

/**
 * 扫描 SD 卡上的 Python 应用
 * @param apps 输出应用数组
 * @param max_count 最大应用数
 * @return 实际扫描到的应用数
 */
int sys_sdcard_scan_apps(app_meta_t *apps, int max_count);

#endif /* SYS_SDCARD_H */
