#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "driver/uart.h"
#include "nvs_flash.h"

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
#include "activity_log.h"
#include "gps_gtu8.h"

#include <sys/time.h>
#include <time.h>
#include "esp_timer.h"

#include "ui/ui.h" // our new UI module
#include "ui/ui_data_page.h"
#include "math.h"
#include "ui/ui_settings_page.h" // Required for ui_settings_register_split_length_cb
#include <stdio.h>
#include "ui_status_bar.h"

static const char *TAG = "app";

#define LOG_QUEUE_LEN 32

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

/* =====================
 * Globals
 * ===================== */

static sd_mmc_helper_t s_sd; 
static QueueHandle_t s_act_q = NULL;
static TaskHandle_t s_act_worker_task = NULL;
static bool s_activity_recording = false;
// Activity/session model + timer
static activity_t s_activity;
static uint32_t s_activity_next_id = 1;

static float s_session_time_s = 0.0f;          // session timer shown on data page
static int64_t s_session_last_us = 0;          // for dt computation

static uint32_t s_last_session_stroke_count = 0; // baseline for session delta
static SemaphoreHandle_t s_activity_mutex = NULL;

/* Activity Log */
static QueueHandle_t s_log_q = NULL;
static activity_log_t s_act_log;

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

/* Last touch point */
static int16_t s_last_touch_x = 0;
static int16_t s_last_touch_y = 0;

/* =====================
 * Forward declarations
 * (group static function prototypes so the implementation order is free)
 * ===================== */

static time_t mktime_utc(struct tm *t);
static void gps_fix_cb(const gps_fix_t *fix, void *user);

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

static void on_shutdown_confirmed(void);
static void app_enter_sleep(void);
static void pwr_evt_cb(pwr_key_event_t evt, void *user);
static void app_pwr_key_setup(void);

static void activity_worker_task(void *arg);
static void on_stop_save_confirmed(void);

static void activity_logger_task(void *arg);

static void on_auto_rotate_setting_changed(bool enabled);
static void on_dark_mode_setting_changed(bool enabled);

static ui_orientation_t decide_orientation_from_accel(float ax, float ay, float az);

static void init_imu(void);
static void stroke_task(void *arg);

static void init_display_and_lvgl(void);
static void init_touch_and_lvgl_input(void);
static esp_err_t app_set_time_from_rtc(void);
static void app_nvs_init(void);

/* ===========================================================
 *  GPS GT-U8 Setup
 * ===========================================================
 */
static bool s_time_synced_from_gps = false;

/* -------------------------------------------------------------------------- */
/*  GPS / Time helpers                                                        */
/* -------------------------------------------------------------------------- */

static time_t mktime_utc(struct tm *t)
{
    // mktime interprets as local time. Convert as UTC by temporarily setting TZ.
    char *old = getenv("TZ");
    char old_copy[64] = {0};
    if (old) strncpy(old_copy, old, sizeof(old_copy)-1);

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(t);

    if (old) setenv("TZ", old_copy, 1);
    else unsetenv("TZ");
    tzset();

    return epoch;
}

static void gps_fix_cb(const gps_fix_t *fix, void *user)
{
    (void)user;
    if (!fix) return;

    if (!s_time_synced_from_gps && fix->valid_time && fix->valid_date) {
        // 1) set system time (epoch in UTC)
        struct tm t = fix->utc_tm;
        time_t epoch_utc = mktime_utc(&t);
        if (epoch_utc > 1700000000) { // sanity check (>= ~2023)
            struct timeval tv = {.tv_sec = epoch_utc, .tv_usec = 0};
            settimeofday(&tv, NULL);

            // 2) convert to local time (Taiwan = UTC+8, no DST)
            setenv("TZ", "CST-8", 1);
            tzset();

            struct tm local_tm;
            localtime_r(&epoch_utc, &local_tm);

            datetime_t dt = {0};
            dt.year   = local_tm.tm_year + 1900;
            dt.month  = local_tm.tm_mon + 1;
            dt.day    = local_tm.tm_mday;
            dt.dotw   = local_tm.tm_wday; // check your RTC expects 0=Sun; adjust if needed
            dt.hour   = local_tm.tm_hour;
            dt.minute = local_tm.tm_min;
            dt.second = local_tm.tm_sec;

            PCF85063_set_all(dt);

            s_time_synced_from_gps = true;
        }
    }
    ESP_LOGI("GPS", "fix=%d time=%d date=%d lat=%.7f lon=%.7f speed=%.2f sats=%d hdop=%.1f",
         fix->valid_fix, fix->valid_time, fix->valid_date,
         fix->lat_deg, fix->lon_deg, fix->speed_mps, fix->sats, fix->hdop);
}


/* -------------------------------------------------------------------------- */
/*  Touch input (LVGL read callback)                                           */
/* -------------------------------------------------------------------------- */

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
/*  Power / Shutdown handling                                                  */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  Power key event handling                                                   */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  Activity worker (start/stop/save)                                          */
/* -------------------------------------------------------------------------- */

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

            if (s_sd.mounted) {
                // Starts the per-stroke CSV log file on the SD card
                activity_log_start(&s_act_log, &s_sd, s_activity.start_ts, s_activity.id);
            }

            s_last_session_stroke_count = 0;

            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);

            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START id=%lu", (unsigned long)id);
        }

        if (cmd == ACT_CMD_STOP_SAVE) {
            s_activity_recording = false;
            
            // Stop logic updates end time and averages
            activity_stop(&s_activity, time(NULL));

            activity_t snapshot = s_activity; 
            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);

            data_page_show_activity_toast(false);
            ESP_LOGI("ACT", "STOP id=%lu Dist=%.1fm", (unsigned long)snapshot.id, (double)snapshot.distance_m);

            // STOP THE LOGGER: This flushes and closes the CSV file.
            // The file is now complete and saved on the SD card.
            activity_log_stop(&s_act_log);

            // REMOVED: activity_save_to_sd(...)
            // We deleted this call because the log file created above IS the save file.
        }
    }
}

static void on_stop_save_confirmed(void)
{
    act_cmd_t cmd = ACT_CMD_STOP_SAVE;
    xQueueSend(s_act_q, &cmd, 0);
}

/* -------------------------------------------------------------------------- */
/*  Logger Tasks                                                              */
/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
/*  Activity logger task                                                       */
/* -------------------------------------------------------------------------- */

static void activity_logger_task(void *arg)
{
    (void)arg;

    activity_log_row_t row;
    for (;;) {
        if (xQueueReceive(s_log_q, &row, portMAX_DELAY) == pdTRUE) {
            // Only append if file is open
            if (s_act_log.opened) {
                activity_log_append(&s_act_log, &row);
            }
        }
    }
}


/* -------------------------------------------------------------------------- */
/*  Settings callbacks                                                         */
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

static void on_split_interval_changed(uint32_t length_m)
{
    ESP_LOGI(TAG, "UI Callback: Split Interval changed to %lu meters", length_m);
    
    // Update the logger configuration immediately
    activity_log_set_split_interval(&s_act_log, length_m);
}

/* -------------------------------------------------------------------------- */
/*  IMU / Orientation helpers                                                  */
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

/* -------------------------------------------------------------------------- */
/*  IMU initialization and stroke detection task                              */
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

    // GPS smoothing state
    static float  s_gps_speed_filt = NAN;
    static double s_gps_lat = NAN;
    static double s_gps_lon = NAN;

    const float fs_hz = 200.0f;
    const stroke_detection_cfg_t cfg = {
        .fs_hz = fs_hz,
        
        // Gravity rejection: Slower is better for a steady hull to isolate "surge" from "tilt"
        .gravity_tau_s = 1.0f,  // Was 0.8f
        
        // Axis detection: Hull acceleration is linear, so we hold the decision longer
        .axis_window_s = 4.0f,
        .axis_hold_s = 1.0f,    // Was 0.5f - reduces switching noise
        .accel_use_fixed_axis = true, // Keep true if you know the mounting orientation
        .accel_fixed_axis = 2,  // Ensure this matches your physical mount (2 = Z-axis usually)
        
        // FILTERS (CRITICAL CHANGE):
        // Boat surge is a slow, rhythmic "push" (approx 0.5 - 1.0 Hz).
        // High frequencies (engine vibration, water chop) must be aggressively cut.
        .hpf_hz = 0.1f,         // Was 0.2f. Needs to pass the very slow drive start.
        .lpf_hz = 3.0f,         // Was 1.2f. Raised slightly to capture the sharp "catch" impact, but still filter vibration.
        
        // TIMING:
        .min_stroke_period_s = 0.8f, // 60 SPM max (Rowing is usually < 40)
        .max_stroke_period_s = 6.0f, // 10 SPM min
        
        // THRESHOLDS:
        // These now apply to Acceleration (m/s^2), not Gyro (rad/s).
        // 1.3x multiplier above noise floor.
        // 0.35 m/s^2 floor (approx 0.035g) avoids triggering on small waves.
        .thr_k = 1.3f,               // Was STROKE_THR_K_DEFAULT (1.0)
        .thr_floor = 0.35f,          // Was STROKE_THR_FLOOR_DEFAULT (0.85)
    };

    stroke_detection_init(&s_stroke, &cfg);

    const int64_t t0_us = esp_timer_get_time();
    int64_t prev_us = esp_timer_get_time();
    TickType_t last_ui_tick = xTaskGetTickCount();

    ui_orientation_t last_orient = s_current_orient;
    int stable_count = 0;

    const TickType_t sample_delay = pdMS_TO_TICKS(5); // ~200 Hz
    const TickType_t ui_period    = pdMS_TO_TICKS(80);  // 12.5 Hz UI updates

    static float s_last_valid_spm = NAN;
    static float s_last_spm_t_s = -1.0f;

    while (1) {
        float ax, ay, az, gx, gy, gz;
        esp_err_t err = qmi8658_read_accel_gyro(&s_imu, &ax, &ay, &az, &gx, &gy, &gz);
        if (err == ESP_OK) {

            int64_t now_us = esp_timer_get_time();
            float t_s  = (float)(now_us - t0_us) * 1e-6f;
            float dt_s = (float)(now_us - prev_us) * 1e-6f;
            prev_us = now_us;
            if (dt_s < 0.0f) dt_s = 0.0f;
            if (dt_s > 0.1f) dt_s = 0.1f;

            stroke_metrics_t m = {0};
            stroke_event_t ev = stroke_detection_update(&s_stroke, t_s, ax, ay, az, gx, gy, gz, &m);

            if (ev != STROKE_EVENT_NONE) {
                ESP_LOGI("STROKE", "ev=%d count=%lu spm=%.1f period=%.2fs",
                         (int)ev, (unsigned long)m.stroke_count, (double)m.spm, (double)m.stroke_period_s);
            }

            // Orientation Logic
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

            if (isfinite(m.spm) && m.spm >= 10.0f && m.spm <= 80.0f) {
                s_last_valid_spm = m.spm;
                s_last_spm_t_s = t_s;
            }

            float spm_raw = s_last_valid_spm;
            if (s_last_spm_t_s > 0.0f && (t_s - s_last_spm_t_s) > 12.0f) spm_raw = NAN;
            if (!isfinite(spm_raw)) spm_raw = 0.0f;

            // GPS Logic
            gps_fix_t fix;
            bool gps_ok = false;
            if (gps_gtu8_get_latest(&fix)) {
                int64_t age_us = esp_timer_get_time() - fix.rx_time_us;
                if (fix.valid_fix && isfinite(fix.speed_mps) && age_us < 2000000) {
                    gps_ok = true;
                    s_gps_lat = fix.lat_deg;
                    s_gps_lon = fix.lon_deg;

                    const float tau = 1.0f;
                    float alpha = dt_s / (tau + dt_s);
                    if (!isfinite(s_gps_speed_filt)) s_gps_speed_filt = fix.speed_mps;
                    else s_gps_speed_filt += alpha * (fix.speed_mps - s_gps_speed_filt);
                }
            }

            float speed_mps = gps_ok ? s_gps_speed_filt : 0.0f;
            float dist_delta_m = speed_mps * dt_s;

            // --- 1. Calculate Derived Metrics for Logging ---
            
            // Instant Pace (s/500m)
            float instant_pace_s = (speed_mps > 0.1f) ? (500.0f / speed_mps) : 0.0f;

            // Stroke Length (m) = Speed * Period
            float stroke_len_m = 0.0f;
            if (isfinite(m.stroke_period_s) && m.stroke_period_s > 0.0f) {
                stroke_len_m = speed_mps * m.stroke_period_s;
            }

            // Recovery Ratio = Recovery / Drive
            float recov_ratio = 0.0f;
            if (m.drive_time_s > 0.01f) {
                recov_ratio = m.recovery_time_s / m.drive_time_s;
            }
            // --- End Derived Metrics ---

            bool need_log = false;
            activity_log_row_t row = {0}; 

            if (s_activity_mutex) xSemaphoreTake(s_activity_mutex, portMAX_DELAY);

            if (s_activity_recording) {
                s_session_time_s += dt_s;

                uint32_t stroke_delta = (ev == STROKE_EVENT_CATCH) ? 1 : 0;

                // Update Session Model (Activity.c)
                activity_update(&s_activity,
                                dt_s,
                                speed_mps,
                                spm_raw,
                                0.0f, // Power placeholder
                                dist_delta_m,
                                stroke_delta);

                // Calculate Avg Pace from Session Avg Speed
                float avg_pace_s = (s_activity.avg_speed_mps > 0.1f) ? (500.0f / s_activity.avg_speed_mps) : 0.0f;

                // Only log on CATCH
                if (ev == STROKE_EVENT_CATCH) {
                    
                    // --- Populate the 16-Column Row ---
                    
                    // 1. RTC Time
                    row.rtc_time = time(NULL); 
                    // 2. Session Time
                    row.session_time_s = s_session_time_s;
                    // 3. Distance (Total)
                    row.total_distance_m = s_activity.distance_m;
                    // 4. Instant Pace
                    row.pace_500m_s = instant_pace_s;
                    // 5. SPM Instant
                    row.spm_instant = spm_raw;
                    // 6. Avg Pace
                    row.avg_pace_500m_s = avg_pace_s;
                    // 7. Avg Speed
                    row.avg_speed_mps = s_activity.avg_speed_mps;
                    // 8. Stroke Length
                    row.stroke_length_m = stroke_len_m;
                    // 9. Stroke Count
                    row.stroke_count = s_activity.stroke_count;
                    // 10. GPS Lat
                    row.gps_lat = gps_ok ? s_gps_lat : 0.0;
                    // 11. GPS Long
                    row.gps_lon = gps_ok ? s_gps_lon : 0.0;
                    // 12. Power
                    row.power_w = 0.0f; 
                    // 13. Drive Time
                    row.drive_time_s = m.drive_time_s;
                    // 14. Recovery Time
                    row.recovery_time_s = m.recovery_time_s;
                    // 15. Recovery Ratio
                    row.recovery_ratio = recov_ratio;

                    need_log = true;
                }
            } else {
                s_session_time_s = 0.0f;
            }

            if (s_activity_mutex) xSemaphoreGive(s_activity_mutex);

            if (need_log && s_log_q) {
                xQueueSend(s_log_q, &row, 0); 
            }

            // UI Update
            TickType_t now = xTaskGetTickCount();
            if ((now - last_ui_tick) >= ui_period)
            {
                last_ui_tick = now;
                float spm_raw_ui = s_last_valid_spm;
                if (s_last_spm_t_s > 0.0f && (t_s - s_last_spm_t_s) > 12.0f) spm_raw_ui = NAN;

                float spm_disp = spm_raw_ui;
                if (isfinite(spm_disp)) spm_disp = ceilf(spm_disp * 2.0f) / 2.0f;

                bool recording = s_activity_recording;
                float pace = (speed_mps > 0.2f) ? (500.0f / speed_mps) : NAN;

                data_values_t v = {
                    .time_s = recording ? s_session_time_s : NAN,
                    .distance_m = recording ? s_activity.distance_m : NAN,
                    .pace_s_per_500m = recording ? pace : NAN,
                    .speed_mps = recording ? speed_mps : NAN,
                    .spm = spm_disp,
                    .power_w = NAN,
                    .stroke_count = recording ? s_activity.stroke_count : UINT32_MAX,
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

/* -------------------------------------------------------------------------- */
/*  Display / LVGL initialization                                              */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  Touch / LVGL input init                                                    */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  RTC / Time helpers                                                         */
/* -------------------------------------------------------------------------- */

static uint8_t calc_dotw(uint16_t y, uint8_t m, uint8_t d)
{
    // Sakamoto: returns 0=Sunday .. 6=Saturday
    static const uint8_t t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    y -= (m < 3);
    return (uint8_t)((y + y/4 - y/100 + y/400 + t[m - 1] + d) % 7);
}

static datetime_t app_default_datetime(void)
{
    datetime_t dt = {
        .year = 2025, .month = 12, .day = 27,
        .hour = 12, .minute = 0, .second = 0,
    };
    dt.dotw = calc_dotw(dt.year, dt.month, dt.day); // 2025-12-27 => 6 (Sat)
    return dt;
}


static esp_err_t app_set_time_from_rtc(void)
{
    // Set timezone to UTC+8 for Taiwan (POSIX TZ sign is reversed)
    setenv("TZ", "CST-8", 1);
    tzset();

    bool valid = false;
    esp_err_t err = PCF85063_is_time_valid(&valid);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC validity check failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!valid) {
    datetime_t def = app_default_datetime();

    ESP_LOGW(TAG,
             "RTC time invalid (OSF set). Seeding RTC to default: %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)def.year, (unsigned)def.month, (unsigned)def.day,
             (unsigned)def.hour, (unsigned)def.minute, (unsigned)def.second);

    esp_err_t se = PCF85063_set_all(def);   // writes full datetime + clears OSF
    if (se != ESP_OK) {
        ESP_LOGW(TAG, "RTC seed failed: %s (system time not updated)", esp_err_to_name(se));
        return se;
    }

    // Now treat as valid and continue to read RTC + set system time
    valid = true;
}

    datetime_t dt;
    err = PCF85063_read_time(&dt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC read failed: %s", esp_err_to_name(err));
        return err;
    }

    struct tm tm_local = {0};
    tm_local.tm_year = (int)dt.year - 1900;
    tm_local.tm_mon  = (int)dt.month - 1;
    tm_local.tm_mday = (int)dt.day;
    tm_local.tm_hour = (int)dt.hour;
    tm_local.tm_min  = (int)dt.minute;
    tm_local.tm_sec  = (int)dt.second;
    tm_local.tm_isdst = -1;

    time_t epoch = mktime(&tm_local);
    if (epoch < 0) {
        ESP_LOGW(TAG, "mktime() failed, not setting system time");
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "System time set from RTC: %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
             (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  NVS / misc helpers                                                         */
/* -------------------------------------------------------------------------- */

static void app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
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
    app_nvs_init();

    gps_gtu8_config_t gps_cfg = {
        .uart_num = UART_NUM_1,
        .tx_gpio = 43,            // board TXD
        .rx_gpio = 44,            // board RXD
        .baud = 9600,             // common GT-U8 default
        .task_prio = 8,
        .task_stack = 4096,
        .rx_buf_size = 2048,
    };
    ESP_ERROR_CHECK(gps_gtu8_init(&gps_cfg));
    ESP_ERROR_CHECK(gps_gtu8_set_callback(gps_fix_cb, NULL));

    app_set_time_from_rtc();
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
    ui_settings_register_split_length_cb(on_split_interval_changed);

    s_activity_mutex = xSemaphoreCreateMutex();
    activity_init(&s_activity, 0);
    s_session_time_s = 0.0f;
    s_session_last_us = esp_timer_get_time();

    s_act_q = xQueueCreate(4, sizeof(act_cmd_t));
    assert(s_act_q);

    s_log_q = xQueueCreate(LOG_QUEUE_LEN, sizeof(activity_log_row_t));
    assert(s_log_q);

    xTaskCreate(activity_logger_task, "activity_logger", 6144, NULL, 6, NULL);
    xTaskCreate(activity_worker_task, "activity_worker", 8192, NULL, 9, &s_act_worker_task);
    xTaskCreatePinnedToCore(stroke_task, "stroke",
                            6144, NULL, 3, NULL, 0);                      

    /* app_main can idle */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
