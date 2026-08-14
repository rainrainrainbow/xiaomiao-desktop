cmake_minimum_required(VERSION 3.16)

# MicroPython Helper 配置
if(NOT MICROPY_HELPER_DIR)
    set(MICROPY_HELPER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/main/managed_components/mr9you__micropython-helper)
endif()

if(EXISTS ${MICROPY_HELPER_DIR})
    include(${MICROPY_HELPER_DIR}/mpy.cmake)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaomiao-desktop)
