#include "lcd_st7789.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "lcd_st7789";

#define LCD_HOST           (CONFIG_LCD_ST7789_HOST == 2 ? SPI2_HOST : SPI3_HOST)

#define PIN_LCD_MOSI       CONFIG_LCD_ST7789_PIN_MOSI
#define PIN_LCD_SCLK       CONFIG_LCD_ST7789_PIN_SCLK
#define PIN_LCD_MISO       CONFIG_LCD_ST7789_PIN_MISO
#define PIN_LCD_DC         CONFIG_LCD_ST7789_PIN_DC
#define PIN_LCD_CS         CONFIG_LCD_ST7789_PIN_CS
#define PIN_LCD_RST        CONFIG_LCD_ST7789_PIN_RST
#define PIN_LCD_BL         CONFIG_LCD_ST7789_PIN_BL

#define LCD_PIXEL_CLOCK_HZ CONFIG_LCD_ST7789_PIXEL_CLOCK
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8

esp_err_t lcd_st7789_init(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io)
{
    ESP_RETURN_ON_FALSE(out_panel, ESP_ERR_INVALID_ARG, TAG, "out_panel is NULL");
    ESP_RETURN_ON_FALSE(out_io,    ESP_ERR_INVALID_ARG, TAG, "out_io is NULL");

    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_GOTO_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                      err, TAG, "spi init failed");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .flags = {
            .dc_low_on_data = 0,
        },
    };
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle),
        err, TAG, "new_panel_io_spi failed");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel),
                      err, TAG, "new_panel_st7789 failed");

    // Backlight
    if (PIN_LCD_BL >= 0) {
        gpio_config_t bk = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << PIN_LCD_BL,
        };
        ESP_ERROR_CHECK(gpio_config(&bk));
        gpio_set_level(PIN_LCD_BL, 1);   // backlight ON
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    *out_panel = panel;
    *out_io = io_handle;
    ESP_LOGI(TAG, "ST7789 LCD initialized");
    return ESP_OK;

err:
    ESP_LOGE(TAG, "lcd init error: %s", esp_err_to_name(ret));
    return ret;
}
