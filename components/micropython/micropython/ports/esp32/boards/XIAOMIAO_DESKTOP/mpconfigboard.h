// 小喵掌机 MicroPython 板级配置
// XiaoMiao Desktop Board Configuration for MicroPython

#ifndef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "XiaoMiao Desktop"
#endif

#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME "ESP32-WROVER-B"
#endif

// 禁用不必要的模块以节省空间
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_ESPNOW                   (0)

// 启用 FAT 文件系统支持
#define MICROPY_VFS_FAT                     (1)

// 禁用 ULP（超低功耗）协处理器
#define MICROPY_PY_MACHINE_ULP              (0)

// 禁用 RMT（远程控制）模块
#define MICROPY_PY_MACHINE_RMT              (0)

// 禁用 PCNT（脉冲计数器）模块
#define MICROPY_PY_MACHINE_PCNT             (0)

// 禁用 SD 卡模块（使用系统已有的 FATFS）
#define MICROPY_PY_MACHINE_SDCARD           (0)

// 禁用 I2S 音频模块
#define MICROPY_PY_MACHINE_I2S              (0)

// 禁用 DAC（数模转换器）
#define MICROPY_PY_MACHINE_DAC              (0)

// 禁用触摸垫
#define MICROPY_PY_MACHINE_TOUCHPAD         (0)