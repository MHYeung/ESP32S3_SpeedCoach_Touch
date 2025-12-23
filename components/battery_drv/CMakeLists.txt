idf_component_register(
    SRCS "battery_drv.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc
)