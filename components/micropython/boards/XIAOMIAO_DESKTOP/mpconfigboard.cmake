# 小喵掌机 MicroPython CMake 配置
# XiaoMiao Desktop Board CMake Configuration for MicroPython

set(IDF_TARGET esp32)

# 板级名称
set(MICROPY_BOARD XIAOMIAO_DESKTOP)

# 分区表（使用 4MB Flash）
set(MICROPY_FROZEN_MANIFEST ${MICROPY_PORT_DIR}/boards/manifest.py)