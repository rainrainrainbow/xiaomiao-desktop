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

// 禁用网络模块（需要 modnetwork.c 等端口文件，暂不集成）
#define MICROPY_PY_NETWORK                  (0)

// 禁用 ADC Block 模块（需要 machine_adc_block.c 等端口文件）
#define MICROPY_PY_MACHINE_ADC_BLOCK        (0)

// 禁用 bitstream 模块（需要 machine_bitstream.c 等端口文件）
#define MICROPY_PY_MACHINE_BITSTREAM        (0)

// 禁用 ESP32 特定模块（需要 esp32_nvs.c、esp32_partition.c 等端口文件）
#define MICROPY_PY_ESP32                    (0)

// 禁用原生代码发射器（需要 esp_native_code_commit 等符号）
#define MICROPY_EMIT_NATIVE                 (0)
#define MICROPY_EMIT_INLINE_THUMB           (0)

// 禁用线程支持（减少依赖）
#define MICROPY_PY_THREAD                   (0)

// 禁用 SSL/TLS（减少依赖）
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_WEBSOCKET                (0)

// 重命名 MicroPython 的 ESP-IDF 入口函数，避免与桌面系统的 app_main 冲突
// 桌面系统通过自定义 API 初始化 MicroPython，不调用此入口
#ifndef MICROPY_ESP_IDF_ENTRY
#define MICROPY_ESP_IDF_ENTRY xiaomiao_mp_entry
#endif