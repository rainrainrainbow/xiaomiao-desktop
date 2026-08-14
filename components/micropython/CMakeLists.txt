# MicroPython ESP32 端口集成
# 注意：这是一个简化版本，仅包含核心功能

set(MICROPY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/micropython)
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)

# 检查 MicroPython 源码是否存在
if(NOT EXISTS ${MICROPY_DIR})
    message(WARNING "MicroPython source not found at ${MICROPY_DIR}")
    message(WARNING "Please clone MicroPython v1.28.0 to components/micropython/micropython")
    return()
endif()

# 包含 ESP32 通用配置
include(${MICROPY_PORT_DIR}/esp32_common.cmake)

# 注册 IDF 组件
idf_component_register(
    SRCS
        # MicroPython 核心源文件将通过 esp32_common.cmake 自动添加
    INCLUDE_DIRS
        ${MICROPY_DIR}
        ${MICROPY_PORT_DIR}
        ${MICROPY_PORT_DIR}/boards/XIAOMIAO_DESKTOP
    REQUIRES
        app_update
        bootloader_support
        bt
        driver
        esp_adc
        esp_app_format
        esp_common
        esp_eth
        esp_event
        esp_hw_support
        esp_netif
        esp_partition
        esp_pm
        esp_psram
        esp_ringbuf
        esp_rom
        esp_system
        esp_timer
        esp_wifi
        freertos
        hal
        heap
        log
        lwip
        mbedtls
        newlib
        nvs_flash
        sdmmc
        soc
        spi_flash
        ulp
        vfs
)

# 设置编译定义
target_compile_definitions(${COMPONENT_TARGET} PUBLIC
    MICROPY_ESP_IDF_4=1
    MICROPY_VFS_FAT=1
    MICROPY_VFS_LFS2=1
)