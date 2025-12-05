idf_component_register(
    SRCS "main.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES lcd_st7789 touch_cst328 esp_lvgl_port i2c_helper qmi8658
)
