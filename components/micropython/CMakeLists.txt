# MicroPython ESP32 端口集成
# 作为 ESP-IDF 组件构建 MicroPython 运行时
# 注意：直接调用 idf_component_register 注册组件

set(MICROPY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/micropython)
set(MICROPY_PORT_DIR ${MICROPY_DIR}/ports/esp32)

# 检查 MicroPython 源码是否存在
if(NOT EXISTS ${MICROPY_DIR})
    message(WARNING "MicroPython source not found at ${MICROPY_DIR}")
    message(WARNING "Please clone MicroPython v1.28.0 to components/micropython/micropython")
    return()
endif()

message(STATUS "MicroPython source found at ${MICROPY_DIR}")

# 设置板级目录
set(MICROPY_BOARD_DIR ${MICROPY_PORT_DIR}/boards/XIAOMIAO_DESKTOP)
set(MICROPY_BOARD XIAOMIAO_DESKTOP)

# 包含 MicroPython 核心构建配置
include(${MICROPY_DIR}/py/py.cmake)
include(${MICROPY_DIR}/extmod/extmod.cmake)

# 设置端口源文件
set(MICROPY_SOURCE_PORT
    ${MICROPY_PORT_DIR}/panichandler.c
    ${MICROPY_PORT_DIR}/gccollect.c
    ${MICROPY_PORT_DIR}/mphalport.c
    ${MICROPY_PORT_DIR}/fatfs_port.c
    ${MICROPY_PORT_DIR}/machine_pin.c
    ${MICROPY_PORT_DIR}/machine_timer.c
    ${MICROPY_PORT_DIR}/machine_hw_spi.c
    ${MICROPY_PORT_DIR}/machine_i2c.c
    ${MICROPY_PORT_DIR}/machine_uart.c
    ${MICROPY_PORT_DIR}/machine_rtc.c
    ${MICROPY_PORT_DIR}/modmachine.c
    ${MICROPY_PORT_DIR}/modesp32.c
    ${MICROPY_PORT_DIR}/modesp.c
    ${MICROPY_PORT_DIR}/uart.c
    ${MICROPY_PORT_DIR}/mpthreadport.c
    ${MICROPY_PORT_DIR}/help.c
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

# 注册组件
idf_component_register(
    SRCS
        ${MICROPY_SOURCE_PY}
        ${MICROPY_SOURCE_EXTMOD}
        ${MICROPY_SOURCE_SHARED}
        ${MICROPY_SOURCE_LIB}
        ${MICROPY_SOURCE_PORT}
    INCLUDE_DIRS
        ${MICROPY_DIR}
        ${MICROPY_DIR}/py
        ${MICROPY_DIR}/extmod
        ${MICROPY_DIR}/shared
        ${MICROPY_PORT_DIR}
        ${MICROPY_BOARD_DIR}
        ${CMAKE_BINARY_DIR}
    REQUIRES
        app_update bootloader_support driver esp_adc esp_app_format esp_common esp_event esp_hw_support esp_mm esp_netif esp_partition esp_psram esp_ringbuf esp_rom esp_system esp_timer freertos hal heap log lwip mbedtls newlib nvs_flash sdmmc soc spi_flash vfs
)

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
    COMMAND cat ${MICROPY_QSTRDEFS_PY} ${MICROPY_QSTRDEFS_COLLECTED} | sed "s/^Q(.*)/\"&\"/" | ${CMAKE_C_COMPILER} -E ${MICROPY_CPP_FLAGS} - | sed "s/^\\\"\\(Q(.*)\\)\\\"/\\1/" > ${MICROPY_QSTRDEFS_PREPROCESSED}
    DEPENDS ${MICROPY_QSTRDEFS_PY}
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

message(STATUS "MicroPython component registered with ${MICROPY_SOURCE_PY} sources")