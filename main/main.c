#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lcd_st7789.h"
#include "touch_cst328.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "i2c_helper.h"
#include "qmi8658.h"
#include "sd_mmc_helper.h"
#include "ble.h"

#include "ui/ui.h" // our new UI module
#include "ui/ui_sd_test_page.h"
#include "ui/ui_data_page.h"
#include "math.h"
#include <stdio.h>

static const char *TAG = "app";
static sd_mmc_helper_t s_sd; // <--- this is the missing variable

/* ---------- Kconfig-based touch pins ---------- */

#define TP_I2C_PORT CONFIG_TOUCH_CST328_I2C_PORT
#define TP_SDA_GPIO CONFIG_TOUCH_CST328_SDA
#define TP_SCL_GPIO CONFIG_TOUCH_CST328_SCL
#define TP_RST_GPIO CONFIG_TOUCH_CST328_RST
#define TP_INT_GPIO CONFIG_TOUCH_CST328_INT
#define TP_I2C_CLK CONFIG_TOUCH_CST328_I2C_CLK

/* IMU I2C pins (board-specific) – can also be Kconfig later */
#define IMU_I2C_PORT CONFIG_IMU_QMI8658_I2C_PORT
#define IMU_SDA_GPIO CONFIG_IMU_QMI8658_SDA
#define IMU_SCL_GPIO CONFIG_IMU_QMI8658_SCL
#define IMU_I2C_CLK CONFIG_IMU_QMI8658_I2C_CLK

/* LVGL display + input */
static lv_disp_t *s_disp = NULL;
static lv_indev_t *s_indev_touch = NULL;

/* IMU handle */
static qmi8658_handle_t s_imu;
static ui_orientation_t s_current_orient = UI_ORIENT_PORTRAIT_0;
static bool s_auto_rotate_enabled = true;

/* Last touch point */
static int16_t s_last_touch_x = 0;
static int16_t s_last_touch_y = 0;

/* ===========================================================
 *  TOUCH → LVGL INPUT CALLBACK
 * ===========================================================
 */

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    cst328_point_t pt;
    if (cst328_read_point(&pt) == ESP_OK && pt.pressed)
    {
        int x = (int)pt.x;
        int y = (int)pt.y;

        if (x < 0)
            x = 0;
        if (x >= LCD_H_RES)
            x = LCD_H_RES - 1;
        if (y < 0)
            y = 0;
        if (y >= LCD_V_RES)
            y = LCD_V_RES - 1;

        s_last_touch_x = x;
        s_last_touch_y = y;

        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = s_last_touch_x;
        data->point.y = s_last_touch_y;
    }
}

static void sd_test_button_action(void)
{
    const char *csv =
        "timestamp_ms,ax,ay,az\n"
        "0,0.01,-0.02,9.81\n"
        "50,0.03,-0.01,9.79\n";

    esp_err_t err = sd_mmc_helper_write_text(&s_sd,
                                             "imu_dummy.csv",
                                             csv,
                                             false); // overwrite
    ESP_LOGI("APP", "sd_mmc_helper_write_text returned %s",
             esp_err_to_name(err));
}

/* -------------------------------------------------------------------------- */
/*  SETTINGS CALLBACKS (from Settings tab)                                    */
/* -------------------------------------------------------------------------- */

static void on_auto_rotate_setting_changed(bool enabled)
{
    s_auto_rotate_enabled = enabled;
    ESP_LOGI("APP", "Auto-rotate %s", enabled ? "ON" : "OFF");
}

static void on_dark_mode_setting_changed(bool enabled)
{
    ESP_LOGI("APP", "Dark mode %s", enabled ? "ON" : "OFF");

    // Simple example: call a UI helper you implement later
    // ui_set_dark_mode(enabled);
}

/* -------------------------------------------------------------------------- */
/*  IMU ORIENTATION HELPER + TASK                                             */
/* -------------------------------------------------------------------------- */

static ui_orientation_t decide_orientation_from_accel(float ax, float ay, float az)
{
    /* Convert to "g" just so thresholds are ~1.0 */
    const float g = 9.80665f;
    float gx = ax / g;
    float gy = ay / g;
    float gz = az / g;

    float ax_abs = fabsf(gx);
    float ay_abs = fabsf(gy);
    float az_abs = fabsf(gz);

    /* If device is almost flat (gravity on Z), don't change orientation */
    if (az_abs > 0.8f)
    {
        return UI_ORIENT_PORTRAIT_0; // we'll treat "no change" separately later
    }

    /* Decide whether it's more "portrait" or "landscape" */
    if (ax_abs > ay_abs)
    {
        /* More tilt in X → landscape */
        if (gx > 0.0f)
        {
            return UI_ORIENT_PORTRAIT_180; // you may swap 90/270 after testing
        }
        else
        {
            return UI_ORIENT_PORTRAIT_0;
        }
    }
    else
    {
        /* More tilt in Y → portrait */
        if (gy > 0.0f)
        {
            return UI_ORIENT_LANDSCAPE_90; // board upside down
        }
        else
        {
            return UI_ORIENT_LANDSCAPE_270; // "normal" portrait
        }
    }
}

static void imu_ui_task(void *arg)
{
    uint32_t sample_idx = 0;
    ui_orientation_t last_decision = s_current_orient;
    int stable_count = 0;

    while (1)
    {
        float ax, ay, az;
        esp_err_t err = qmi8658_read_accel(&s_imu, &ax, &ay, &az);
        if (s_auto_rotate_enabled)
        {
            ui_orientation_t candidate = decide_orientation_from_accel(ax, ay, az);

            /* Simple stability filter */
            if (candidate == last_decision)
            {
                if (stable_count < 20)
                    stable_count++; // cap it
            }
            else
            {
                last_decision = candidate;
                stable_count = 0;
            }

            /* Only change orientation if it's been stable for N samples */
            const int REQUIRED_STABLE_SAMPLES = 8; // 8 * 50ms = 400ms
            if (stable_count >= REQUIRED_STABLE_SAMPLES &&
                candidate != s_current_orient)
            {

                s_current_orient = candidate;
                ui_set_orientation(candidate);
            }
        }
        if (err == ESP_OK)
        {
            ui_update_imu(ax, ay, az);

            /* Light logging: every 10th sample to avoid UART lag */
            if ((sample_idx++ % 10) == 0)
            {
                ESP_LOGI("IMU", "ax=%.2f ay=%.2f az=%.2f m/s^2", ax, ay, az);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // 20 Hz
    }
}

/* ===========================================================
 *  INIT HELPERS
 * ===========================================================
 */

static void init_display_and_lvgl(void)
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
        .buffer_size = LCD_H_RES * 40,
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
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    lv_disp_set_default(s_disp);
}

static void init_touch_and_lvgl_input(void)
{
    ESP_LOGI(TAG, "Init CST328 touch (Kconfig pins)...");

    ESP_ERROR_CHECK(cst328_init(TP_I2C_PORT,
                                TP_SDA_GPIO,
                                TP_SCL_GPIO,
                                TP_RST_GPIO,
                                TP_INT_GPIO,
                                TP_I2C_CLK));

    s_indev_touch = lv_indev_create();
    lv_indev_set_type(s_indev_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev_touch, touch_read_cb);
    lv_indev_set_display(s_indev_touch, s_disp);
}

static void init_imu_and_task(void)
{
    ESP_LOGI(TAG, "Init IMU I2C bus + QMI8658...");

    i2c_helper_t imu_bus;
    ESP_ERROR_CHECK(i2c_helper_init(&imu_bus,
                                    IMU_I2C_PORT,
                                    IMU_SDA_GPIO,
                                    IMU_SCL_GPIO,
                                    IMU_I2C_CLK));

    ESP_ERROR_CHECK(qmi8658_init(&s_imu, &imu_bus, QMI8658_I2C_ADDR));

    /* IMU UI task – prio 3, LVGL task is usually prio 4 */
    xTaskCreatePinnedToCore(imu_ui_task, "imu_ui",
                            4096, NULL, 3, NULL, 0);
}

/* ===========================================================
 *  app_main – orchestrator
 * ===========================================================
 */

void app_main(void)
{
    init_display_and_lvgl();
    init_touch_and_lvgl_input();

    /* Create UI in separate module */
    ui_init(s_disp);

    ESP_ERROR_CHECK(ble_app_init());
    ble_set_device_name("ESP32S3-BLE"); // optional custom name
    ble_start_advertising();

    /* Mount SD card */
    ESP_ERROR_CHECK(sd_mmc_helper_mount(&s_sd, "/sdcard"));

    ui_register_dark_mode_cb(on_dark_mode_setting_changed);
    ui_register_auto_rotate_cb(on_auto_rotate_setting_changed);

    const char *test = "hello\n";
    esp_err_t err = sd_mmc_helper_write_text(&s_sd, "hello.txt", test, false);
    ESP_LOGI("APP", "initial hello.txt write returned %s", esp_err_to_name(err));

    ui_register_sd_test_cb(sd_test_button_action);

    /* IMU in its own task */
    init_imu_and_task();

    /* app_main can idle */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
