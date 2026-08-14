cmake_minimum_required(VERSION 3.16)

# 显式添加 micropython 组件目录（确保 ESP-IDF 发现它）
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components/micropython)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaomiao-desktop)
