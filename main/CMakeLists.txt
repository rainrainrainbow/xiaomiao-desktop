idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd
                  esp_driver_gpio
                  esp_driver_spi
                  esp_driver_ledc
                  esp_timer
                  app_update
    REQUIRES driver
)
target_link_libraries(${COMPONENT_LIB} PRIVATE m)