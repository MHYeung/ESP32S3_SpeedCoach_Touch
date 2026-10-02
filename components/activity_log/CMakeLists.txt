idf_component_register(
    SRCS "activity_log.c"
    INCLUDE_DIRS "include"
    REQUIRES sd_mmc_helper activity sensor_hub
)