# MicroPython ESP32 端口集成
# 作为 ESP-IDF 组件构建 MicroPython 运行时
# 注意：esp32_common.cmake 内部会调用 idf_component_register 注册组件

set(MICROPY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/micropython)
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)

# 检查 MicroPython 源码是否存在
if(NOT EXISTS ${MICROPY_DIR})
    message(WARNING "MicroPython source not found at ${MICROPY_DIR}")
    message(WARNING "Please clone MicroPython v1.28.0 to components/micropython/micropython")
    return()
endif()

# 设置板级目录
set(MICROPY_BOARD_DIR ${MICROPY_PORT_DIR}/boards/XIAOMIAO_DESKTOP)
set(MICROPY_BOARD XIAOMIAO_DESKTOP)

# 包含 ESP32 通用配置（内部会调用 idf_component_register 注册组件）
include(${MICROPY_PORT_DIR}/esp32_common.cmake)