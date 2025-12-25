#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"

#include "lcd_st7789.h"
#include "touch_cst328.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "i2c_helper.h"
#include "qmi8658.h"
#include "sd_mmc_helper.h"
#include "ble.h"
#include "stroke_detection.h"
#include "rtc_pcf85063.h"
#include "battery_drv.h"
#include "pwr_key.h"
#include "activity.h"

#include "esp_timer.h"

#include "ui/ui.h" // our new UI module
#include "ui/ui_data_page.h"
#include "math.h"
#include <stdio.h>
#include "ui_status_bar.h"

static const char *TAG = "app";

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

/* Toggle Activity globals*/
typedef enum {
    ACT_CMD_START = 0,
    ACT_CMD_STOP_SAVE,
} act_cmd_t;

static sd_mmc_helper_t s_sd; 
static QueueHandle_t s_act_q = NULL;
static TaskHandle_t s_act_worker_task = NULL;
static bool s_activity_recording = false;
// Activity/session model + timer
static activity_t s_activity;
static uint32_t s_activity_next_id = 1;

static float s_session_time_s = 0.0f;          // session timer shown on data page
static int64_t s_session_last_us = 0;          // for dt computation

static uint32_t s_last_stroke_count_seen = 0;  // latest raw stroke counter from detector (boot-based)
static uint32_t s_last_session_stroke_count = 0; // baseline for session delta

static SemaphoreHandle_t s_activity_mutex = NULL;

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

/* ===========================================================
 *  PWR_KEY Setup
 * ===========================================================
 */


static void on_shutdown_confirmed(void)
{
    // Optional: save state, flush logs, stop peripherals, etc.
    // Then cut the latch power:
    pwr_key_set_hold(false);
}

static void app_enter_sleep(void)
{
    // TODO: turn off backlight here if you have a function/pin for it
    // backlight_set(false);

    // Wake on key press (active-low)
    // GPIO6 must be RTC-capable for ext0; on ESP32-S3 this is usually OK.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_6, 0);

    // Light sleep keeps RAM and resumes quickly
    esp_light_sleep_start();

    // Woke up
    // backlight_set(true);
}

static void pwr_evt_cb(pwr_key_event_t evt, void *user)
{
    (void)user;

    switch (evt) {
        case PWR_KEY_EVT_ACTIVITY_TOGGLE:
        {
            if (!s_activity_recording) {
                // Start immediately
                act_cmd_t cmd = ACT_CMD_START;
                xQueueSend(s_act_q, &cmd, 0);
            } else {
                // Recording -> ask user
                ui_show_stop_save_prompt();
            }
            break;
        }

        case PWR_KEY_EVT_SHUTDOWN_PROMPT:
            // Keep your previous behavior (optional)
            ui_show_shutdown_prompt();
            ESP_LOGI(TAG, "Long press 5s: show shutdown prompt");
            break;

        case PWR_KEY_EVT_SHORT_PRESS:
        default:
            // optional: ignore short press for now
            break;
    }
}

static void app_pwr_key_setup(void)
{
    // Start the power key task
    pwr_key_config_t cfg = {
        .key_gpio = GPIO_NUM_6,
        .hold_gpio = GPIO_NUM_7,
        .key_active_low = true,
        .debounce_ms = 30,
        .poll_ms = 20,
        .click_max_ms = 600,
        //.toggle_hold_ms = 1600,
        .prompt_hold_ms = 3000,
    };
    ESP_ERROR_CHECK(pwr_key_init(&cfg, pwr_evt_cb, NULL));

    // Keep power latched on
    pwr_key_set_hold(true);
}

static void activity_worker_task(void *arg)
{
    (void)arg;

    act_cmd_t cmd;

    for (;;) {
        if (xQueueReceive(s_act_q, &cmd, portMAX_DELAY) != pdTRUE) continue;

        ui_go_to_page(UI_PAGE_DATA, true);

        if (s_activity_mutex) xSemaphoreTake(s_activity_mutex, portMAX_DELAY);

        if (cmd == ACT_CMD_START) {
            s_activity_recording = true;
            s_session_time_s = 0.0f;

            uint32_t id = s_activity_next_id++;
            activity_init(&s_activity, id);
            activity_start(&s_activity, time(NULL));

            // baseline will be set by stroke_task on first cycle
            s_last_session_stroke_count = 0;

            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);

            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START id=%lu", (unsigned long)id);
        }

        if (cmd == ACT_CMD_STOP_SAVE) {
             s_activity_recording = false;
            activity_stop(&s_activity, time(NULL));

            activity_t snapshot = s_activity; // copy for saving
            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);

            data_page_show_activity_toast(false);
            ESP_LOGI("ACT", "STOP id=%lu", (unsigned long)snapshot.id);

            if (s_sd.mounted) {
                esp_err_t e = activity_save_to_sd(&s_sd, &snapshot, true);
                ESP_LOGI("ACT", "Saved: %s", esp_err_to_name(e));
            } else {
                ESP_LOGW("ACT", "SD not mounted, not saved");
            }
        }
    }
}

static void on_stop_save_confirmed(void)
{
    act_cmd_t cmd = ACT_CMD_STOP_SAVE;
    xQueueSend(s_act_q, &cmd, 0);
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

static void stroke_task(void *arg)
{
    (void)arg;

    const float fs_hz = 200.0f;
    const stroke_detection_cfg_t cfg = {
        .fs_hz = fs_hz,
        .gravity_tau_s = 0.8f,
        .axis_window_s = 4.0f,
        .axis_hold_s = 0.5f,
        .accel_use_fixed_axis = true,
        .accel_fixed_axis = 2,
        .hpf_hz = 0.2f,
        .lpf_hz = 1.2f,
        .min_stroke_period_s = 0.8f,
        .max_stroke_period_s = 5.0f,
        .thr_k = STROKE_THR_K_DEFAULT,
        .thr_floor = STROKE_THR_FLOOR_DEFAULT,
    };

    stroke_detection_init(&s_stroke, &cfg);

    const int64_t t0_us = esp_timer_get_time();
    int64_t prev_us = esp_timer_get_time();
    TickType_t last_ui_tick = xTaskGetTickCount();

    ui_orientation_t last_orient = s_current_orient;
    int stable_count = 0;

    const TickType_t sample_delay = pdMS_TO_TICKS(5); // ~200 Hz
    const TickType_t ui_period    = pdMS_TO_TICKS(100);  // 10 Hz UI updates

    static float s_last_valid_spm = NAN;
    static float s_last_spm_t_s = -1.0f;

    // Keep a “latest” detector count so START can baseline
    static uint32_t s_latest_detector_strokes = 0;

    while (1) {
        float ax, ay, az, gx, gy, gz;
        esp_err_t err = qmi8658_read_accel_gyro(&s_imu, &ax, &ay, &az, &gx, &gy, &gz);
        if (err == ESP_OK) {

            // Use one timestamp per loop for consistency
            int64_t now_us = esp_timer_get_time();
            float t_s  = (float)(now_us - t0_us) * 1e-6f;         // boot time (for algorithm)
            float dt_s = (float)(now_us - prev_us) * 1e-6f;       // delta time
            prev_us = now_us;
            if (dt_s < 0.0f) dt_s = 0.0f;
            if (dt_s > 0.1f) dt_s = 0.1f; // clamp in case of pauses

            // Stroke detection
            stroke_metrics_t m = {0};
            stroke_event_t ev = stroke_detection_update(&s_stroke, t_s, ax, ay, az, gx, gy, gz, &m);

            // Track latest detector stroke count
            s_latest_detector_strokes = m.stroke_count;

            // Optional debug
            if (ev != STROKE_EVENT_NONE) {
                ESP_LOGI("STROKE", "ev=%d count=%lu spm=%.1f period=%.2fs",
                         (int)ev, (unsigned long)m.stroke_count, (double)m.spm, (double)m.stroke_period_s);
            }

            // Orientation auto-rotate (OK to keep; consider running this at 10Hz later)
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

            // Keep last valid SPM (bounded + integer output)
            if (isfinite(m.spm) && m.spm >= 10.0f && m.spm <= 80.0f) {
                s_last_valid_spm = m.spm;
                s_last_spm_t_s = t_s;
            }

            // ---- Activity logic (session timer + activity model) ----
            float ui_time_s = 0.0f;
            uint32_t ui_strokes = 0;

            // If you’ll later add GPS speed/distance: fill these in here
            float speed_mps = 0.0f;
            float power_w   = 0.0f;
            float dist_delta_m = 0.0f;

            if (s_activity_mutex) xSemaphoreTake(s_activity_mutex, portMAX_DELAY);

            if (s_activity_recording) {
                // Session time counts only when recording
                s_session_time_s += dt_s;

                // Baseline stroke count on first recording cycle
                if (s_last_session_stroke_count == 0) {
                    s_last_session_stroke_count = s_latest_detector_strokes;
                }

                uint32_t stroke_delta = 0;
                if (s_latest_detector_strokes >= s_last_session_stroke_count) {
                    stroke_delta = s_latest_detector_strokes - s_last_session_stroke_count;
                }
                s_last_session_stroke_count = s_latest_detector_strokes;

                float spm_raw = (isfinite(s_last_valid_spm) ? s_last_valid_spm : NAN);
                float spm_for_act = (isfinite(spm_raw) ? spm_raw : 0.0f);
                //float spm_for_act = (isfinite(s_last_valid_spm) ? s_last_valid_spm : 0.0f);

                // Update activity summary stats
                activity_update(&s_activity,
                                dt_s,
                                speed_mps,
                                spm_for_act,
                                power_w,
                                dist_delta_m,
                                stroke_delta);
            } else {
                // Not recording: keep at 0 (or freeze last session if you prefer)
                // Here we keep it at 0 as you requested.
                s_session_time_s = 0.0f;
                s_last_session_stroke_count = 0;
            }

            ui_time_s  = s_session_time_s;
            ui_strokes = s_activity.stroke_count;

            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);
            // ---- End activity logic ----

            // ---- UI update at 10Hz ----
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ui_tick) >= ui_period)
            {
                last_ui_tick = now;

                float spm_raw = s_last_valid_spm;
                if (s_last_spm_t_s > 0.0f && (t_s - s_last_spm_t_s) > 12.0f) {
                    spm_raw = NAN;
                }

                // Display value: snap to 0.5 increments (choose ONE rounding behavior)
                float spm_disp = spm_raw;
                if (isfinite(spm_disp)) {
                    // Nearest 0.5 step:
                    //spm_disp = roundf(spm_disp * 2.0f) / 2.0f;

                    // If you want CEIL to next 0.5 (what you currently have):
                    spm_disp = ceilf(spm_disp * 2.0f) / 2.0f;

                    // If you want FLOOR to lower 0.5:
                    // spm_disp = floorf(spm_disp * 2.0f) / 2.0f;
                }

                bool recording = s_activity_recording;

                data_values_t v = {
                    .time_s = recording ? s_session_time_s : NAN,                 // hide when not recording
                    .distance_m = NAN,
                    .pace_s_per_500m = NAN,
                    .speed_mps = NAN,
                    .spm = spm_disp,                                               // <-- keep visible ALWAYS
                    .power_w = NAN,
                    .stroke_count = recording ? s_activity.stroke_count : UINT32_MAX, // hide when not recording
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
    init_imu();
    PCF85063_init(&s_imu_bus);
    app_pwr_key_setup();

    /* Create UI in separate module */
    ui_init(s_disp);
    ui_set_orientation(s_current_orient);

    /* Default Data page metrics for rowing */
    const data_metric_t metrics[3] = {
        DATA_METRIC_TIME,
        DATA_METRIC_STROKE_COUNT,
        DATA_METRIC_SPM,
    };
    data_page_set_metrics(metrics, 3);

    ESP_ERROR_CHECK(ble_app_init());
    ble_set_device_name("ESP32S3-BLE"); // optional custom name
    ble_start_advertising();

    esp_err_t sd_err = sd_mmc_helper_mount(&s_sd, "/sdcard");
    if (sd_err != ESP_OK)
    {
        ESP_LOGW(TAG, "SD mount failed: %s (continuing)", esp_err_to_name(sd_err));
    }

    ui_register_dark_mode_cb(on_dark_mode_setting_changed);
    ui_register_auto_rotate_cb(on_auto_rotate_setting_changed);
    ui_register_shutdown_confirm_cb(on_shutdown_confirmed);
    ui_register_stop_save_confirm_cb(on_stop_save_confirmed);

    s_activity_mutex = xSemaphoreCreateMutex();
    activity_init(&s_activity, 0);
    s_session_time_s = 0.0f;
    s_session_last_us = esp_timer_get_time();

    s_act_q = xQueueCreate(4, sizeof(act_cmd_t));
    assert(s_act_q);

    xTaskCreate(activity_worker_task, "activity_worker", 8192, NULL, 9, &s_act_worker_task);
    xTaskCreatePinnedToCore(stroke_task, "stroke",
                            6144, NULL, 3, NULL, 0);

    /* app_main can idle */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
