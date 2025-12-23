idf_component_register(
    SRCS "rtc_pcf85063.c"
    INCLUDE_DIRS "include"
    REQUIRES i2c_helper driver
)