#ifndef MPCONFIGBOARD_H
#define MPCONFIGBOARD_H

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
#define MICROPY_HW_ENABLE_SDCARD            (0)

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
// 禁用 ADC 模块（端口实现依赖 adc_block 符号）
#define MICROPY_PY_MACHINE_ADC              (0)

// 禁用 bitstream 模块（需要 machine_bitstream.c 等端口文件）
#define MICROPY_PY_MACHINE_BITSTREAM        (0)

// 禁用 ESP32 特定模块（需要 esp32_nvs.c、esp32_partition.c 等端口文件）
#define MICROPY_PY_ESP32                    (0)

// 禁用原生代码发射器（需要 esp_native_code_commit 等符号）
#define MICROPY_EMIT_NATIVE                 (0)
#define MICROPY_EMIT_INLINE_THUMB           (0)
#define MICROPY_EMIT_XTENSAWIN              (0)
#define MICROPY_PERSISTENT_CODE_LOAD        (0)

// 禁用线程支持（减少依赖）
#define MICROPY_PY_THREAD                   (0)

// 禁用 SSL/TLS（减少依赖）
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_WEBSOCKET                (0)

// 禁用 DHT 传感器（需要 dht_readinto_obj）
#define MICROPY_PY_MACHINE_DHT              (0)
#define MICROPY_PY_MACHINE_DHT_READINTO     (0)

// 禁用脉冲测量（需要 machine_time_pulse_us_obj）
#define MICROPY_PY_MACHINE_PULSE            (0)

// 禁用 NLR jump fail（避免 nlr_jump_fail 未定义）
#define MICROPY_NLR_SETJMP                  (1)

// 重命名 MicroPython 的 ESP-IDF 入口函数，避免与桌面系统的 app_main 冲突
// 桌面系统通过自定义 API 初始化 MicroPython，不调用此入口
#ifndef MICROPY_ESP_IDF_ENTRY
#define MICROPY_ESP_IDF_ENTRY xiaomiao_mp_entry
#endif

// ============================================================
// 强制覆盖：以下宏在 mpconfigport.h 中可能被重新定义为 (1)，
// 但这些模块需要被禁用以避免链接器错误。
// 我们使用 #undef + #define 确保这些值生效。
// 注意：这些必须在 mpconfigport.h 的 #include 之后被处理，
// 但由于 mpconfigport.h 会在包含本文件后继续处理，本文件
// 末尾的 #undef+#define 可能仍会被覆盖。因此我们使用
// 预处理指令技巧：通过 #include 重新定义。
// ============================================================

// 实际上，这些宏必须在 mpconfigport.h 中的 #define 之后生效。
// 让 build.yml 中的编译器 -D 标志覆盖它们是最可靠的方案。
// 请参考 build.yml 中的 EXTRA_CFLAGS 设置。

#endif // MPCONFIGBOARD_H
