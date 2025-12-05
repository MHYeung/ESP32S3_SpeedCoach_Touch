#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif


#define LCD_H_RES 240
#define LCD_V_RES 320

esp_err_t lcd_st7789_init(esp_lcd_panel_handle_t *out_panel,
                          esp_lcd_panel_io_handle_t *out_io);

#ifdef __cplusplus
}
#endif
