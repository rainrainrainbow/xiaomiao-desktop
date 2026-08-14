# MicroPython ESP32 端口集成
# 作为 ESP-IDF 组件构建 MicroPython 运行时
#
# 重要说明：
# ESP-IDF 会分两个阶段处理组件 CMakeLists.txt：
#   1. 需求收集阶段（component_get_requirements.cmake）：仅提取 REQUIRES 依赖
#   2. 实际构建阶段：真正编译组件
# 在需求收集阶段，COMPONENT_LIB 尚未定义为 target，因此所有依赖 target 的
# 构建逻辑（target_include_directories / target_compile_definitions 等）
# 都必须放在 if(TARGET ${COMPONENT_LIB}) 守卫内，否则会失败。
#
# 关键点：idf_component_register 必须传入 SRCS，否则组件会被创建为
# INTERFACE library，无法添加 PRIVATE 源文件/编译定义。

message(STATUS "MICROPYTHON COMPONENT: Loading micropython component")

# 使用 CMAKE_CURRENT_LIST_DIR 获取当前 CMakeLists.txt 所在目录（绝对路径）
# 这是最可靠的方式：在需求收集阶段和实际构建阶段都指向组件真实目录
# 注意：PROJECT_SOURCE_DIR 在需求收集阶段为空，CMAKE_CURRENT_SOURCE_DIR 会被解析为 build/ 目录
set(MICROPY_COMPONENT_DIR ${CMAKE_CURRENT_LIST_DIR})
set(MICROPY_DIR ${MICROPY_COMPONENT_DIR}/micropython)
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)
message(STATUS "MICROPY_COMPONENT_DIR = ${MICROPY_COMPONENT_DIR}")

# 检查 MicroPython 源码是否存在
if(NOT EXISTS ${MICROPY_DIR})
    message(WARNING "MicroPython source not found at ${MICROPY_DIR}")
    message(WARNING "Please clone MicroPython v1.28.0 to components/micropython/micropython")
endif()

message(STATUS "MicroPython source found at ${MICROPY_DIR}")

# 设置板级目录
set(MICROPY_BOARD_DIR ${MICROPY_PORT_DIR}/boards/XIAOMIAO_DESKTOP)
set(MICROPY_BOARD XIAOMIAO_DESKTOP)

# 包含 MicroPython 核心构建配置（设置 MICROPY_SOURCE_PY / MICROPY_SOURCE_EXTMOD 等变量）
# 必须在 idf_component_register 之前，这样 SRCS 才能引用这些源文件列表，
# 使组件被创建为 STATIC library（而非 INTERFACE library）
include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/extmod/extmod.cmake)

# ============================================================
# 从未实现的模块中移除源文件（避免链接器 undefined reference 错误）
# ============================================================
# 注意：这些模块虽然在 mpconfigboard.h 中被禁用（宏定义为 0），
# 但它们的源文件仍在 MICROPY_SOURCE_EXTMOD 列表中，CMake 仍会编译它们。
# 由于条件编译排除了函数实现，但保留了函数声明，导致链接器找不到实现。

# 移除未实现的 extmod 模块源文件
list(REMOVE_ITEM MICROPY_SOURCE_EXTMOD
    ${MICROPY_EXTMOD_DIR}/machine_adc.c            # ADC（端口实现依赖 adc_block 符号）
    ${MICROPY_EXTMOD_DIR}/machine_adc_block.c      # ADC Block（需要 esp_driver_adc 支持）
    ${MICROPY_EXTMOD_DIR}/machine_bitstream.c       # Bitstream（需要 GPIO bitbang 实现）
    ${MICROPY_EXTMOD_DIR}/modnetwork.c              # Network 模块（需要 lwIP/cyw43 等后端）
    ${MICROPY_EXTMOD_DIR}/network_cyw43.c           # CYW43 WiFi/BT 驱动
    ${MICROPY_EXTMOD_DIR}/network_lwip.c            # LWIP 网络后端
    ${MICROPY_EXTMOD_DIR}/network_ninaw10.c         # Nina-W10 WiFi 模块
    ${MICROPY_EXTMOD_DIR}/network_ppp_lwip.c        # PPP over LWIP
    ${MICROPY_EXTMOD_DIR}/network_wiznet5k.c        # Wiznet5k 以太网芯片
    ${MICROPY_EXTMOD_DIR}/modbluetooth.c            # Bluetooth 模块
    ${MICROPY_EXTMOD_DIR}/btstack/modbluetooth_btstack.c  # BTstack 后端
    ${MICROPY_EXTMOD_DIR}/nimble/modbluetooth_nimble.c    # NimBLE 后端
    ${MICROPY_EXTMOD_DIR}/modtls_axtls.c            # axtls TLS 后端（未使用）
    ${MICROPY_EXTMOD_DIR}/modwebrepl.c              # WebREPL（需要 network）
    ${MICROPY_EXTMOD_DIR}/modwebsocket.c            # WebSocket（需要 network）
)

message(STATUS "Removed unimplemented extmod modules from source list")
message(STATUS "MICROPY_SOURCE_EXTMOD now has ${CMAKE_MATCH_COUNT} files")

# 设置端口源文件
# 注意：machine_uart.c 和 modmachine.c 是 INCLUDEFILE（通过
# MICROPY_PY_MACHINE_UART_INCLUDEFILE / MICROPY_PY_MACHINE_INCLUDEFILE
# 被 extmod/machine_uart.c 和 extmod/machine.c 包含），不能单独编译，
# 因此不列入此列表，否则会因缺少 machine_uart_type 等声明而编译失败。
#
# 已移除的未实现模块源文件（避免链接器 undefined reference 错误）：
# - modesp32.c: ESP32 模块主文件（引用 esp32_nvs_type, esp32_partition_type 等）
# - esp32_nvs.c, esp32_partition.c, esp32_pcnt.c, esp32_rmt.c, esp32_ulp.c
# - machine_sdcard.c, machine_touchpad.c
set(MICROPY_SOURCE_PORT
    ${MICROPY_PORT_DIR}/panichandler.c
    ${MICROPY_PORT_DIR}/gccollect.c
    ${MICROPY_PORT_DIR}/mphalport.c
    ${MICROPY_PORT_DIR}/fatfs_port.c
    ${MICROPY_PORT_DIR}/machine_pin.c
    ${MICROPY_PORT_DIR}/machine_timer.c
    ${MICROPY_PORT_DIR}/machine_hw_spi.c
    ${MICROPY_PORT_DIR}/machine_i2c.c
    ${MICROPY_PORT_DIR}/machine_rtc.c
    # ${MICROPY_PORT_DIR}/modesp32.c          # 已移除：引用未实现的 ESP32 模块类型
    ${MICROPY_PORT_DIR}/modesp.c
    ${MICROPY_PORT_DIR}/uart.c
    ${MICROPY_PORT_DIR}/mpthreadport.c
    ${MICROPY_PORT_DIR}/help.c
    # 以下文件已移除，因为它们的符号被 modesp32.c 引用但未实现：
    # ${MICROPY_PORT_DIR}/esp32_nvs.c         # NVS 模块
    # ${MICROPY_PORT_DIR}/esp32_partition.c   # Partition 模块
    # ${MICROPY_PORT_DIR}/esp32_pcnt.c        # PCNT 模块
    # ${MICROPY_PORT_DIR}/esp32_rmt.c         # RMT 模块
    # ${MICROPY_PORT_DIR}/esp32_ulp.c         # ULP 模块
    # ${MICROPY_PORT_DIR}/machine_sdcard.c    # SD 卡模块（受 MICROPY_HW_ENABLE_SDCARD 保护）
    ${MICROPY_PORT_DIR}/machine_touchpad.c     # TouchPad 模块（被 modmachine.c 引用，需保留）
)

# 设置共享源文件
set(MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/readline/readline.c
    ${MICROPY_DIR}/shared/netutils/netutils.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
    ${MICROPY_DIR}/shared/runtime/interrupt_char.c
    ${MICROPY_DIR}/shared/runtime/mpirq.c
    ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/runtime/pyexec.c
)

# 设置 lib 源文件
set(MICROPY_SOURCE_LIB
    ${MICROPY_DIR}/lib/littlefs/lfs1.c
    ${MICROPY_DIR}/lib/littlefs/lfs1_util.c
    ${MICROPY_DIR}/lib/littlefs/lfs2.c
    ${MICROPY_DIR}/lib/littlefs/lfs2_util.c
    ${MICROPY_DIR}/lib/oofatfs/ff.c
    ${MICROPY_DIR}/lib/oofatfs/ffunicode.c
)

# 设置 drivers 源文件（softspi 等）
set(MICROPY_SOURCE_DRIVERS
    ${MICROPY_DIR}/drivers/bus/softspi.c
)

# 注册组件（两个阶段都会执行，用于记录 REQUIRES 和创建 STATIC library）
idf_component_register(
    SRCS
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_LIB}
        ${MICROPY_SOURCE_DRIVERS}
        ${MICROPY_SOURCE_PORT}
    INCLUDE_DIRS
        ${MICROPY_DIR}
        ${MICROPY_DIR}/py
        ${MICROPY_DIR}/extmod
        ${MICROPY_DIR}/shared
        ${MICROPY_DIR}/drivers/bus
        ${MICROPY_PORT_DIR}
        ${MICROPY_BOARD_DIR}
        ${CMAKE_BINARY_DIR}
    REQUIRES
        app_update bootloader_support driver esp_adc esp_app_format esp_common esp_driver_dac esp_driver_gpio esp_driver_gptimer esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_mcpwm esp_driver_pcnt esp_driver_rmt esp_driver_sdmmc esp_driver_sdspi esp_driver_spi esp_driver_uart esp_event esp_hw_support esp_mm esp_netif esp_partition esp_pm esp_psram esp_ringbuf esp_rom esp_system esp_timer esp_wifi freertos hal heap log lwip mbedtls newlib nvs_flash sdmmc soc spi_flash vfs
)

# ============================================================
# 以下构建逻辑仅在"实际构建阶段"执行
# （需求收集阶段 COMPONENT_LIB 尚未定义为 target）
# ============================================================
if(TARGET ${COMPONENT_LIB})

# 设置编译定义
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MICROPY_VFS_FAT=1
    MICROPY_VFS_LFS2=1
    FFCONF_H="${MICROPY_DIR}/lib/oofatfs/ffconf.h"
    LFS1_NO_MALLOC LFS1_NO_DEBUG LFS1_NO_WARN LFS1_NO_ERROR LFS1_NO_ASSERT
    LFS2_NO_MALLOC LFS2_NO_DEBUG LFS2_NO_WARN LFS2_NO_ERROR LFS2_NO_ASSERT
)

# 禁用一些警告
target_compile_options(${COMPONENT_LIB} PUBLIC
    -Wno-clobbered
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
)

# ========== qstr 生成规则 ==========
set(MICROPY_GENHDR_DIR "${CMAKE_BINARY_DIR}/genhdr")
set(MICROPY_MPVERSION "${MICROPY_GENHDR_DIR}/mpversion.h")
set(MICROPY_QSTRDEFS_PY "${MICROPY_DIR}/py/qstrdefs.h")
set(MICROPY_QSTRDEFS_PORT "${MICROPY_PORT_DIR}/qstrdefsport.h")
set(MICROPY_QSTRDEFS_LAST "${MICROPY_GENHDR_DIR}/qstr.i.last")
set(MICROPY_QSTRDEFS_SPLIT "${MICROPY_GENHDR_DIR}/qstr.split")
set(MICROPY_QSTRDEFS_COLLECTED "${MICROPY_GENHDR_DIR}/qstrdefs.collected.h")
set(MICROPY_QSTRDEFS_PREPROCESSED "${MICROPY_GENHDR_DIR}/qstrdefs.preprocessed.h")
set(MICROPY_QSTRDEFS_GENERATED "${MICROPY_GENHDR_DIR}/qstrdefs.generated.h")
set(MICROPY_MODULEDEFS_SPLIT "${MICROPY_GENHDR_DIR}/moduledefs.split")
set(MICROPY_MODULEDEFS_COLLECTED "${MICROPY_GENHDR_DIR}/moduledefs.collected")
set(MICROPY_MODULEDEFS "${MICROPY_GENHDR_DIR}/moduledefs.h")
set(MICROPY_ROOT_POINTERS_SPLIT "${MICROPY_GENHDR_DIR}/root_pointers.split")
set(MICROPY_ROOT_POINTERS_COLLECTED "${MICROPY_GENHDR_DIR}/root_pointers.collected")
set(MICROPY_ROOT_POINTERS "${MICROPY_GENHDR_DIR}/root_pointers.h")

# 收集 qstr 源文件
set(MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_LIB}
    ${MICROPY_SOURCE_PORT}
)

# 计算预处理器标志
get_target_property(MICROPY_CPP_INC ${COMPONENT_LIB} INCLUDE_DIRECTORIES)
get_target_property(MICROPY_CPP_DEF ${COMPONENT_LIB} COMPILE_DEFINITIONS)

# 收集 REQUIRES 组件的 include 路径（ESP-IDF 系统头文件如 esp_random.h 等）
# 通过 micropy_gather_target_properties 宏（定义于 py.cmake）从链接库收集
get_target_property(_mp_link_libs ${COMPONENT_LIB} INTERFACE_LINK_LIBRARIES)
foreach(_lib ${_mp_link_libs})
    micropy_gather_target_properties(${_lib})
endforeach()
list(APPEND MICROPY_CPP_INC ${MICROPY_CPP_INC_EXTRA})

set(_prefix "-I")
foreach(_arg ${MICROPY_CPP_INC})
    list(APPEND MICROPY_CPP_FLAGS ${_prefix}${_arg})
endforeach()
set(_prefix "-D")
foreach(_arg ${MICROPY_CPP_DEF})
    list(APPEND MICROPY_CPP_FLAGS ${_prefix}${_arg})
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 生成 mpversion.h
add_custom_target(
    BUILD_VERSION_HEADER ALL
    BYPRODUCTS ${MICROPY_MPVERSION}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MICROPY_GENHDR_DIR}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeversionhdr.py ${MICROPY_MPVERSION}
)

# 生成 qstrdefs.generated.h
add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_LAST}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py pp ${CMAKE_C_COMPILER} -E output ${MICROPY_GENHDR_DIR}/qstr.i.last cflags ${MICROPY_CPP_FLAGS} -DNO_QSTR cxxflags ${MICROPY_CPP_FLAGS} -DNO_QSTR sources ${MICROPY_SOURCE_QSTR}
    DEPENDS ${MICROPY_MPVERSION}
        ${MICROPY_SOURCE_QSTR}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py split qstr ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/qstr _
    COMMAND touch ${MICROPY_QSTRDEFS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py cat qstr _ ${MICROPY_GENHDR_DIR}/qstr ${MICROPY_QSTRDEFS_COLLECTED}
    BYPRODUCTS "${MICROPY_QSTRDEFS_COLLECTED}.hash"
    DEPENDS ${MICROPY_QSTRDEFS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_PREPROCESSED}
    COMMAND cat ${MICROPY_QSTRDEFS_PY} ${MICROPY_QSTRDEFS_PORT} ${MICROPY_QSTRDEFS_COLLECTED} | sed "s/^Q(.*)/\"&\"/" | ${CMAKE_C_COMPILER} -E ${MICROPY_CPP_FLAGS} - | sed "s/^\\\"\\(Q(.*)\\)\\\"/\\1/" > ${MICROPY_QSTRDEFS_PREPROCESSED}
    DEPENDS ${MICROPY_QSTRDEFS_PY}
        ${MICROPY_QSTRDEFS_PORT}
        ${MICROPY_QSTRDEFS_COLLECTED}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_QSTRDEFS_GENERATED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdata.py ${MICROPY_QSTRDEFS_PREPROCESSED} > ${MICROPY_QSTRDEFS_GENERATED}
    DEPENDS ${MICROPY_QSTRDEFS_PREPROCESSED}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

# 生成 moduledefs.h
add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py split module ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/module _
    COMMAND touch ${MICROPY_MODULEDEFS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py cat module _ ${MICROPY_GENHDR_DIR}/module ${MICROPY_MODULEDEFS_COLLECTED}
    BYPRODUCTS "${MICROPY_MODULEDEFS_COLLECTED}.hash"
    DEPENDS ${MICROPY_MODULEDEFS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_MODULEDEFS}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makemoduledefs.py ${MICROPY_MODULEDEFS_COLLECTED} > ${MICROPY_MODULEDEFS}
    DEPENDS ${MICROPY_MODULEDEFS_COLLECTED}
)

# 生成 root_pointers.h
add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS_SPLIT}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py split root_pointer ${MICROPY_GENHDR_DIR}/qstr.i.last ${MICROPY_GENHDR_DIR}/root_pointer _
    COMMAND touch ${MICROPY_ROOT_POINTERS_SPLIT}
    DEPENDS ${MICROPY_QSTRDEFS_LAST}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS_COLLECTED}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/makeqstrdefs.py cat root_pointer _ ${MICROPY_GENHDR_DIR}/root_pointer ${MICROPY_ROOT_POINTERS_COLLECTED}
    BYPRODUCTS "${MICROPY_ROOT_POINTERS_COLLECTED}.hash"
    DEPENDS ${MICROPY_ROOT_POINTERS_SPLIT}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

add_custom_command(
    OUTPUT ${MICROPY_ROOT_POINTERS}
    COMMAND ${Python3_EXECUTABLE} ${MICROPY_DIR}/py/make_root_pointers.py ${MICROPY_ROOT_POINTERS_COLLECTED} > ${MICROPY_ROOT_POINTERS}
    DEPENDS ${MICROPY_ROOT_POINTERS_COLLECTED} ${MICROPY_DIR}/py/make_root_pointers.py
)

# 将生成的头文件加入 target sources
target_sources(${COMPONENT_LIB} PRIVATE
    ${MICROPY_MPVERSION}
    ${MICROPY_QSTRDEFS_GENERATED}
    ${MICROPY_MODULEDEFS}
    ${MICROPY_ROOT_POINTERS}
)

# 确保生成的头文件被构建（触发 add_custom_command 链）
add_custom_target(MICROPY_GENHDR ALL
    DEPENDS
        ${MICROPY_MPVERSION}
        ${MICROPY_QSTRDEFS_GENERATED}
        ${MICROPY_MODULEDEFS}
        ${MICROPY_ROOT_POINTERS}
)

# ========== 引脚生成规则（生成 genhdr/pins.h 和 pins.c） ==========
set(GEN_PINS_PREFIX "${MICROPY_PORT_DIR}/boards/pins_prefix.c")
set(GEN_PINS_MKPINS "${MICROPY_PORT_DIR}/boards/make-pins.py")
set(GEN_PINS_SRC "${CMAKE_BINARY_DIR}/pins.c")
set(GEN_PINS_HDR "${MICROPY_GENHDR_DIR}/pins.h")
if(EXISTS "${MICROPY_BOARD_DIR}/pins.csv")
    set(GEN_PINS_BOARD_CSV "${MICROPY_BOARD_DIR}/pins.csv")
    set(GEN_PINS_BOARD_CSV_ARG --board-csv "${GEN_PINS_BOARD_CSV}")
endif()
target_sources(${COMPONENT_LIB} PRIVATE ${GEN_PINS_SRC} ${GEN_PINS_HDR})

add_custom_command(
    OUTPUT ${GEN_PINS_SRC} ${GEN_PINS_HDR}
    COMMAND ${Python3_EXECUTABLE} ${GEN_PINS_MKPINS} ${GEN_PINS_BOARD_CSV_ARG}
        --prefix ${GEN_PINS_PREFIX} --output-source ${GEN_PINS_SRC} --output-header ${GEN_PINS_HDR}
    DEPENDS
        ${MICROPY_MPVERSION}
        ${GEN_PINS_MKPINS}
        ${GEN_PINS_PREFIX}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

# 将 pins.h 加入 MICROPY_GENHDR 依赖，确保生成
add_custom_target(MICROPY_GENPINS ALL
    DEPENDS ${GEN_PINS_HDR}
)

endif()  # if(TARGET ${COMPONENT_LIB}) — 需求收集阶段跳过构建逻辑

message(STATUS "MicroPython component registered with ${MICROPY_SOURCE_PY} sources")