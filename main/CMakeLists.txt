idf_component_register(
    SRCS 
        "main.c"
        "ui/ui_framework.c"
        "ui/event_bus.c"
        "app/app_manager.c"
        "app/app_builtin.c"
        "app/app_micropython.c"
        "driver/drv_button.c"
        "driver/drv_backlight.c"
        "driver/drv_battery.c"
        "system/sys_nvs.c"
        "return_to_loader.c"
        "fonts/lv_font_xiaomiao_cn_14.c"
    INCLUDE_DIRS "." "ui" "app" "driver" "system" "fonts"
    REQUIRES driver esp_adc esp_lcd lvgl nvs_flash esp_timer
)

# MicroPython 头文件路径（源码由 CI 下载到 components/micropython/micropython）
target_include_directories( PUBLIC
    /../components/micropython/micropython
    /../components/micropython/micropython/py
    /../components/micropython/micropython/extmod
    /../components/micropython/micropython/shared
    /../components/micropython/micropython/ports/esp32
    /../components/micropython/micropython/ports/esp32/boards/XIAOMIAO_DESKTOP
)
