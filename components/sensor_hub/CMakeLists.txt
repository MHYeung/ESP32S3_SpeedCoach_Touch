idf_component_register(
    SRCS "sensor_hub.c" "sensor_hub_hrs.c" "sensor_hub_rowpod.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES bt nvs_flash nvs_helper esp_timer
)
