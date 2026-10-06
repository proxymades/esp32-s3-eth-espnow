idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_timer
        esp_lcd
        esp_driver_spi
)