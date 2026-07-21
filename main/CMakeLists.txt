idf_component_register(
    SRCS 
        main.c
        return_to_loader.c
    INCLUDE_DIRS .
    REQUIRES esp_adc esp_timer
)
