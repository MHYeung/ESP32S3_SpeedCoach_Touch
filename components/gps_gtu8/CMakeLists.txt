idf_component_register(
    SRCS "gps_gtu8.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer
)
