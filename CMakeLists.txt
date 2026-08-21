cmake_minimum_required(VERSION 3.16)

# 显式添加 micropython 组件目录（确保 ESP-IDF 发现它）
# 使用 CMAKE_CURRENT_LIST_DIR 获取顶层 CMakeLists.txt 所在目录的绝对路径
# 注意：EXTRA_COMPONENT_DIRS 中的路径必须是绝对路径，否则 ESP-IDF 会将其解析为 build/ 下的相对路径
set(EXTRA_COMPONENT_DIRS 
    ${CMAKE_CURRENT_LIST_DIR}/components/micropython
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaomiao-desktop)
