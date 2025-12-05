#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lcd_st7789.h"
#include "touch_cst328.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "app";

static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_indev_touch = NULL;

/* Cached state to make tapping smoother */
static bool s_last_pressed = false;
static int16_t s_last_x = 0;
static int16_t s_last_y = 0;


/* Forward decl. */
static void create_demo_ui(void);
static void touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data);

void app_main(void)
{
    ESP_LOGI(TAG, "Init ST7789 via esp_lcd...");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(lcd_st7789_init(&panel_handle, &io_handle));

    ESP_LOGI(TAG, "Init LVGL port...");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    ESP_LOGI(TAG, "Add LVGL display...");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 40,        // ~1/8 of screen, good starting point
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);

    /* Optional: set default display for LVGL */
    lv_disp_set_default(s_disp);
    
    ESP_LOGI(TAG, "Init touch (Kconfig pins)...");
    i2c_port_t port = CONFIG_TOUCH_CST328_I2C_PORT;
    gpio_num_t sda  = CONFIG_TOUCH_CST328_SDA;
    gpio_num_t scl  = CONFIG_TOUCH_CST328_SCL;
    gpio_num_t rst  = CONFIG_TOUCH_CST328_RST;
    gpio_num_t irq  = CONFIG_TOUCH_CST328_INT;
    uint32_t clk    = CONFIG_TOUCH_CST328_I2C_CLK;
    ESP_ERROR_CHECK(cst328_init(port, sda, scl, rst, irq, clk));

    ESP_LOGI(TAG, "Register LVGL input device (pointer)...");
    s_indev_touch = lv_indev_create();
    lv_indev_set_type(s_indev_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev_touch, touch_read_cb);
    lv_indev_set_display(s_indev_touch, s_disp);

    ESP_LOGI(TAG, "Create LVGL demo UI...");
    lvgl_port_lock(0);         // lock LVGL core (port requirement)
    create_demo_ui();
    lvgl_port_unlock();

    // Nothing else to do here: LVGL runs in its own task created by lvgl_port_init()
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}

static void touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    (void) indev;

    cst328_point_t pt;
    if (cst328_read_point(&pt) == ESP_OK && pt.pressed) {
        int x = (int)pt.x;
        int y = (int)pt.y;

        // Clamp to LVGL resolution
        if (x < 0) x = 0;
        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y < 0) y = 0;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;

        s_last_pressed = true;
        s_last_x = x;
        s_last_y = y;

        data->state    = LV_INDEV_STATE_PRESSED;
        data->point.x  = x;
        data->point.y  = y;
    } else {
        // No touch this cycle – keep last coordinates, mark released
        data->state    = LV_INDEV_STATE_RELEASED;
        data->point.x  = s_last_x;
        data->point.y  = s_last_y;
        s_last_pressed = false;
    }
}

static void btn_event_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);

    const char * txt = lv_label_get_text(label);
    if (strcmp(txt, "Click me") == 0) {
        lv_label_set_text(label, "Touched!");
    } else {
        lv_label_set_text(label, "Click me");
    }
}

static void create_demo_ui(void)
{
    lv_obj_t * scr = lv_disp_get_scr_act(s_disp);

    // Background color
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Title label
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "Hello LVGL + CST328");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

    // Button in middle
    lv_obj_t * btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click me");
    lv_obj_center(btn_label);
}


