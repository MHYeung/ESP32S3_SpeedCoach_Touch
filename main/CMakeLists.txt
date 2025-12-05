idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES lcd_st7789 touch_cst328 esp_lvgl_port
)
