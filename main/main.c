#include "freertos/FreeRTOS.h"
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
#include "stroke_detection.h"
#include "esp_timer.h"

#include "ui/ui.h" // our new UI module
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
static i2c_helper_t s_imu_bus;
static stroke_detection_t s_stroke;
/* Change this to pick a fixed UI orientation at boot */
static ui_orientation_t s_current_orient = UI_ORIENT_LANDSCAPE_270;
static bool s_auto_rotate_enabled = false;
static bool s_auto_rotate_locked = false;

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

/* -------------------------------------------------------------------------- */
/*  IMU INIT + STROKE TASK                                                    */
/* -------------------------------------------------------------------------- */

static void init_imu(void)
{
    ESP_LOGI(TAG, "Init IMU I2C bus + QMI8658...");

    ESP_ERROR_CHECK(i2c_helper_init(&s_imu_bus,
                                    IMU_I2C_PORT,
                                    IMU_SDA_GPIO,
                                    IMU_SCL_GPIO,
                                    IMU_I2C_CLK));

    ESP_ERROR_CHECK(qmi8658_init(&s_imu, &s_imu_bus, QMI8658_I2C_ADDR));
}

static void auto_rotate_on_stroke_update(stroke_event_t ev, float t_s)
{
    static float last_event_t = -1.0f;

    if (ev != STROKE_EVENT_NONE) {
        last_event_t = t_s;
        if (!s_auto_rotate_locked && s_auto_rotate_enabled) {
            s_auto_rotate_locked = true;
            s_auto_rotate_enabled = false;
            ESP_LOGI("APP", "Auto-rotate locked (stroke started)");
        }
    }

    // Unlock after a quiet period (no stroke events)
    const float UNLOCK_IDLE_S = 5.0f;
    if (s_auto_rotate_locked && last_event_t > 0.0f && (t_s - last_event_t) > UNLOCK_IDLE_S) {
        s_auto_rotate_locked = false;
        s_auto_rotate_enabled = true;
        ESP_LOGI("APP", "Auto-rotate unlocked (idle %.1fs)", (double)(t_s - last_event_t));
    }
}

static void stroke_task(void *arg)
{
    (void)arg;

    const float fs_hz = 200.0f;
    const stroke_detection_cfg_t cfg = {
        .fs_hz = fs_hz,
        .gravity_tau_s = 0.8f,
        .axis_window_s = 4.0f,
        .axis_hold_s = 0.5f,
        .hpf_hz = 0.5f,
        .lpf_hz = 10.0f,
        .min_stroke_period_s = 0.4f,
        .max_stroke_period_s = 6.0f,
        .thr_k = 1.2f,
        .thr_floor = 0.5f,
    };

    stroke_detection_init(&s_stroke, &cfg);

    const int64_t t0_us = esp_timer_get_time();
    TickType_t last_ui_tick = xTaskGetTickCount();

    ui_orientation_t last_orient = s_current_orient;
    int stable_count = 0;

    const TickType_t sample_delay = pdMS_TO_TICKS(5);   // ~200 Hz
    const TickType_t ui_period = pdMS_TO_TICKS(100);    // 10 Hz UI updates

    while (1) {
        float ax, ay, az, gx, gy, gz;
        esp_err_t err = qmi8658_read_accel_gyro(&s_imu, &ax, &ay, &az, &gx, &gy, &gz);
        if (err == ESP_OK) {
            float t_s = (float)(esp_timer_get_time() - t0_us) * 1e-6f;

            stroke_metrics_t m;
            stroke_event_t ev = stroke_detection_update(&s_stroke, t_s, ax, ay, az, gx, gy, gz, &m);
            auto_rotate_on_stroke_update(ev, t_s);
            if (ev != STROKE_EVENT_NONE) {
                ESP_LOGI("STROKE", "ev=%d spm=%.1f drive=%.2fs rec=%.2fs",
                         (int)ev, (double)m.spm, (double)m.drive_time_s, (double)m.recovery_time_s);
            }

            if (s_auto_rotate_enabled) {
                ui_orientation_t candidate = decide_orientation_from_accel(ax, ay, az);

                if (candidate == last_orient) {
                    if (stable_count < 20) stable_count++;
                } else {
                    last_orient = candidate;
                    stable_count = 0;
                }

                if (stable_count >= 8 && candidate != s_current_orient) {
                    s_current_orient = candidate;
                    ui_set_orientation(candidate);
                }
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_ui_tick) >= ui_period) {
                last_ui_tick = now;

                data_values_t v = {
                    .time_s = t_s,
                    .distance_m = NAN,
                    .pace_s_per_500m = NAN,
                    .speed_mps = NAN,
                    .spm = m.spm,
                    .stroke_period_s = m.stroke_period_s,
                    .drive_time_s = m.drive_time_s,
                    .recovery_time_s = m.recovery_time_s,
                    .power_w = NAN,
                };
                data_page_set_values(&v);
            }
        }

        vTaskDelay(sample_delay);
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
    ui_set_orientation(s_current_orient);

    /* Default Data page metrics for rowing */
    const data_metric_t metrics[3] = {
        DATA_METRIC_SPM,
        DATA_METRIC_DRIVE_TIME,
        DATA_METRIC_RECOVERY_TIME,
    };
    data_page_set_metrics(metrics, 3);

    ESP_ERROR_CHECK(ble_app_init());
    ble_set_device_name("ESP32S3-BLE"); // optional custom name
    ble_start_advertising();

    /* Optional SD card */
    esp_err_t sd_err = sd_mmc_helper_mount(&s_sd, "/sdcard");
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (continuing)", esp_err_to_name(sd_err));
    }

    ui_register_dark_mode_cb(on_dark_mode_setting_changed);
    ui_register_auto_rotate_cb(on_auto_rotate_setting_changed);

    init_imu();
    xTaskCreatePinnedToCore(stroke_task, "stroke",
                            6144, NULL, 3, NULL, 0);

    /* app_main can idle */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
