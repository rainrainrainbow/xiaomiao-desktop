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
    INCLUDE_DIRS "." "ui" "app" "driver" "system"
    PRIV_REQUIRES esp_lcd
                  esp_timer
                  esp_driver_gpio
                  esp_driver_spi
                  esp_driver_ledc
                  esp_driver_adc
                  esp_hw_support
                  heap
                  spi_flash
                  lvgl
                  nvs_flash
                  app_update
    REQUIRES driver
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)