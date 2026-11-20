/**
 * @file app_recovery.h
 * @brief 恢复模式（Recovery）页面头文件
 *
 * 类似安卓 Recovery 的维护菜单，提供恢复出厂设置、清除 WiFi 凭据、
 * OTA 升级、SD 卡刷机提示、系统信息、重启到主系统/Loader 等能力。
 */
#ifndef APP_RECOVERY_H
#define APP_RECOVERY_H

#include "ui_framework.h"

/**
 * 恢复模式页面回调（通过 ui_stack_push 进入）
 */
extern const page_callbacks_t g_recovery_callbacks;

#endif /* APP_RECOVERY_H */
