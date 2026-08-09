idf_component_register(
    SRCS 
        "main.c"
        "ui/ui_framework.c"
        "app/app_manager.c"
        "app/app_builtin.c"
        "app/app_micropython.c"
        "driver/drv_button.c"
        "driver/drv_backlight.c"
        "driver/drv_battery.c"
        "system/sys_nvs.c"
        "return_to_loader.c"
    INCLUDE_DIRS "." "ui" "app" "driver" "system"
    REQUIRES driver esp_adc esp_lcd lvgl nvs_flash esp_timer
)