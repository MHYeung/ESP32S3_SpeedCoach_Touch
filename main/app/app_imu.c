#include "app_imu.h"

#include "app_activity.h"
#include "app_context.h"
#include "app_pins.h"
#include "coach_ui_snapshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "gps_gtu8.h"
#include "interval_program.h"
#include "math.h"
#include "race_program.h"
#include "ui.h"
#include "ui_interval_data_page.h"
#include "qmi8658.h"
#include "sensor_hub.h"
#include "stroke_detection.h"

#include <time.h>
#include <string.h>

static const char *TAG = "app";
static uint32_t s_phase_hr_sum;
static uint16_t s_phase_hr_n;

static uint16_t take_phase_hr_avg(void)
{
    uint16_t avg = 0;
    if (s_phase_hr_n > 0) {
        avg = (uint16_t)(s_phase_hr_sum / s_phase_hr_n);
    }
    s_phase_hr_sum = 0;
    s_phase_hr_n = 0;
    return avg;
}

static sensor_link_t sensor_link_from_live(const sensor_hub_live_t *live)
{
    if (live->hr_bpm > 0) {
        return SENSOR_LINK_LIVE;
    }
    if (live->hr_saved && !live->hr_paused) {
        return SENSOR_LINK_LOST;
    }
    return SENSOR_LINK_NONE;
}
static volatile bool s_session_reset = false;
static uint32_t s_ui_seq = 0;

/* Stroke period window, shared by the detector config and the SPM sanity gate
 * so a catch can never be accepted at a spacing the SPM filter rejects.
 * 0.6 s = 100 SPM, 6.0 s = 10 SPM. */
#define STROKE_MIN_PERIOD_S 0.6f
#define STROKE_MAX_PERIOD_S 6.0f
#define STROKE_SPM_MIN (60.0f / STROKE_MAX_PERIOD_S)
#define STROKE_SPM_MAX (60.0f / STROKE_MIN_PERIOD_S)

void app_imu_request_session_reset(void)
{
    s_session_reset = true;
}

static const char *interval_unit_to_csv(interval_unit_t unit)
{
    switch (unit)
    {
    case INTERVAL_UNIT_TIME:
        return "s";
    case INTERVAL_UNIT_DISTANCE:
        return "m";
    case INTERVAL_UNIT_STROKES:
        return "st";
    default:
        return "";
    }
}

static const char *interval_phase_to_str(interval_phase_t phase)
{
    return (phase == INTERVAL_PHASE_WORK) ? "WORK" : "REST";
}

static void build_interval_row(activity_log_interval_row_t *row,
                               interval_phase_t phase,
                               uint16_t round_idx,
                               const interval_target_t *target,
                               float phase_time_s,
                               float phase_dist_m,
                               uint32_t phase_strokes)
{
    if (!row)
        return;

    if (phase_time_s < 0.0f)
        phase_time_s = 0.0f;
    if (phase_dist_m < 0.0f)
        phase_dist_m = 0.0f;

    memset(row, 0, sizeof(*row));
    row->round_index = round_idx;
    strncpy(row->phase, interval_phase_to_str(phase), sizeof(row->phase) - 1);
    if (target)
    {
        row->target_value = target->value;
        strncpy(row->target_unit, interval_unit_to_csv(target->unit), sizeof(row->target_unit) - 1);
    }
    row->phase_time_s = phase_time_s;
    row->phase_distance_m = phase_dist_m;
    row->phase_pace_s = (phase_dist_m > 0.1f) ? (phase_time_s / (phase_dist_m / 500.0f)) : 0.0f;
    row->avg_spm = (phase_time_s > 0.1f) ? ((float)phase_strokes * 60.0f / phase_time_s) : 0.0f;
}

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
        /* More tilt in X -> landscape */
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
        /* More tilt in Y -> portrait */
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

void init_imu(void)
{
    ESP_LOGI(TAG, "Init IMU I2C bus + QMI8658...");

    ESP_ERROR_CHECK(i2c_helper_init(&s_imu_bus,
                                    IMU_I2C_PORT,
                                    IMU_SDA_GPIO,
                                    IMU_SCL_GPIO,
                                    IMU_I2C_CLK));

    ESP_ERROR_CHECK(qmi8658_init(&s_imu, &s_imu_bus, QMI8658_I2C_ADDR));
}

void stroke_task(void *arg)
{
    (void)arg;

    // GPS smoothing state
    static float s_gps_speed_filt = NAN;
    static double s_gps_lat = NAN;
    static double s_gps_lon = NAN;
    static float s_split_anchor_m = 0.0f;
    static float s_split_anchor_t = 0.0f;
    static float s_last_split_pace = NAN;
    static float s_last_split_delta = NAN;

    static bool s_interval_prev_recording = false;
    static bool s_interval_phase_active = false;
    static interval_phase_t s_interval_log_phase = INTERVAL_PHASE_IDLE;
    static uint16_t s_interval_log_round = 0;
    static float s_interval_phase_start_time_s = 0.0f;
    static float s_interval_phase_start_dist_m = 0.0f;
    static uint32_t s_interval_phase_start_strokes = 0;
    static bool s_interval_cfg_valid = false;
    static interval_config_t s_interval_log_cfg = {0};

    /* Must stay an exact multiple of the FreeRTOS tick (10 ms @ 100 Hz), or the
     * pacing delay truncates to 0 and the loop free-runs at the I2C rate while
     * the detector's filter coefficients still assume fs_hz. */
    const float fs_hz = 100.0f;
    const TickType_t sample_delay = pdMS_TO_TICKS((int)(1000.0f / fs_hz));
    _Static_assert(pdMS_TO_TICKS(10) >= 1, "tick rate too low to pace the IMU loop");
    const stroke_detection_cfg_t cfg = {
        .fs_hz = fs_hz,

        // Gravity rejection: Slower is better for a steady hull to isolate "surge" from "tilt"
        .gravity_tau_s = 1.0f, // Was 0.8f

        // Axis detection: Hull acceleration is linear, so we hold the decision longer
        .axis_window_s = 2.0f,
        .axis_hold_s = 1.0f,
        /* Auto-select the highest-variance axis: the deck mount can put surge on
         * X, Y or Z, and a fixed axis goes deaf to motion in the other two. */
        .accel_use_fixed_axis = false,
        .accel_fixed_axis = 2,

        .hpf_hz = 0.1f,
        .lpf_hz = 3.0f,

        /* Debounce must not be looser than the period window, or a catch can be
         * accepted at a spacing that the SPM window then throws away. */
        .min_stroke_period_s = STROKE_MIN_PERIOD_S,
        .max_stroke_period_s = STROKE_MAX_PERIOD_S,
        .min_catch_interval_s = STROKE_MIN_PERIOD_S,

        .thr_k = 1.8f,
        .thr_floor = 1.2f, /* ~0.12 g: above resting noise, below a real catch */
    };

    stroke_detection_init(&s_stroke, &cfg);

    int64_t t0_us = esp_timer_get_time();
    int64_t prev_us = t0_us;

    float s_last_valid_spm = NAN;
    float s_last_spm_t_s = -1.0f;

    const TickType_t ui_period = pdMS_TO_TICKS(100); // 10Hz
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_ui_tick = xTaskGetTickCount();

    ui_orientation_t last_orient = s_current_orient;
    int stable_count = 0;

    while (1)
    {
        if (s_session_reset)
        {
            s_session_reset = false;
            s_gps_speed_filt = NAN;
            s_gps_lat = NAN;
            s_gps_lon = NAN;
            s_split_anchor_m = 0.0f;
            s_split_anchor_t = 0.0f;
            s_last_split_pace = NAN;
            s_last_split_delta = NAN;
        }

        float ax, ay, az, gx, gy, gz;
        if (qmi8658_read_accel_gyro(&s_imu, &ax, &ay, &az, &gx, &gy, &gz) == ESP_OK)
        {
            int64_t now_us = esp_timer_get_time();
            float t_s = (float)(now_us - t0_us) * 1e-6f;
            float dt_s = (float)(now_us - prev_us) * 1e-6f;
            prev_us = now_us;
            if (dt_s < 0.0f)
                dt_s = 0.0f;
            if (dt_s > 0.1f)
                dt_s = 0.1f;

            stroke_metrics_t m = {0};
            stroke_event_t ev = stroke_detection_update(&s_stroke, t_s, ax, ay, az, gx, gy, gz, &m);

            if (ev != STROKE_EVENT_NONE)
            {
                ESP_LOGI("STROKE", "ev=%d count=%lu spm=%.1f period=%.2fs axis=%d",
                         (int)ev, (unsigned long)m.stroke_count, (double)m.spm,
                         (double)m.stroke_period_s, s_stroke.best_axis);
            }

            // Orientation Logic
            if (s_auto_rotate_enabled)
            {
                ui_orientation_t candidate = decide_orientation_from_accel(ax, ay, az);
                if (candidate == last_orient)
                {
                    if (stable_count < 20)
                        stable_count++;
                }
                else
                {
                    last_orient = candidate;
                    stable_count = 0;
                }
                if (stable_count >= 8 && candidate != s_current_orient)
                {
                    s_current_orient = candidate;
                    ui_set_orientation(candidate);
                }
            }

            if (isfinite(m.spm) && m.spm >= STROKE_SPM_MIN && m.spm <= STROKE_SPM_MAX)
            {
                s_last_valid_spm = m.spm;
                s_last_spm_t_s = t_s;
            }

            float spm_raw = s_last_valid_spm;
            if (s_last_spm_t_s > 0.0f && (t_s - s_last_spm_t_s) > 12.0f)
                spm_raw = NAN;
            if (!isfinite(spm_raw))
                spm_raw = 0.0f;

            // GPS Logic
            gps_fix_t fix;
            bool gps_connected = false;
            uint8_t gps_bars = 0; // 0 means "no fix / searching"
            bool gps_ok = false;

            if (gps_gtu8_get_latest(&fix))
            {
                int64_t age_us = esp_timer_get_time() - fix.rx_time_us;
                gps_connected = (age_us < 2000000);

                if (gps_connected && fix.valid_fix)
                {
                    // ok for UI quality
                    int sats = fix.sats;
                    float hdop = fix.hdop;

                    if (sats >= 10 && hdop <= 1.2f)
                        gps_bars = 4;
                    else if (sats >= 8 && hdop <= 1.8f)
                        gps_bars = 3;
                    else if (sats >= 6 && hdop <= 2.8f)
                        gps_bars = 2;
                    else
                        gps_bars = 1;
                }
                else if (gps_connected)
                {
                    gps_bars = 0; // connected but no fix -> RED dot
                }

                // your gps_ok logic can stay stricter:
                    if (gps_connected && fix.valid_fix && isfinite(fix.speed_mps))
                {
                    gps_ok = true;
                    s_gps_lat = fix.lat_deg;
                    s_gps_lon = fix.lon_deg;

                    const float tau = 2.2f;
                    float alpha = dt_s / (tau + dt_s);
                    if (!isfinite(s_gps_speed_filt))
                        s_gps_speed_filt = fix.speed_mps;
                    else
                        s_gps_speed_filt += alpha * (fix.speed_mps - s_gps_speed_filt);
                }
            }
            else
            {
                gps_connected = false;
                gps_bars = 0;
            }

            float speed_mps = gps_ok ? s_gps_speed_filt : 0.0f;
            float dist_delta_m = speed_mps * dt_s;

            // --- 1. Calculate Derived Metrics for Logging ---

            // Instant Pace (s/500m)
            float instant_pace_s = (speed_mps > 0.1f) ? (500.0f / speed_mps) : 0.0f;

            // Stroke Length (m) = Speed * Period
            float stroke_len_m = 0.0f;
            if (isfinite(m.stroke_period_s) && m.stroke_period_s > 0.0f)
            {
                stroke_len_m = speed_mps * m.stroke_period_s;
            }

            // Recovery Ratio = Recovery / Drive
            float recov_ratio = 0.0f;
            if (m.drive_time_s > 0.01f)
            {
                recov_ratio = m.recovery_time_s / m.drive_time_s;
            }
            // --- End Derived Metrics ---

            bool need_log = false;
            bool need_interval_log = false;
            bool interval_was_recording = s_interval_prev_recording;
            activity_log_row_t row = {0};
            activity_log_interval_row_t interval_row = {0};

            if (s_activity_mutex)
                xSemaphoreTake(s_activity_mutex, portMAX_DELAY);

            if (s_activity_recording)
            {
                s_session_time_s = (s_session_last_us > 0)
                    ? (float)(esp_timer_get_time() - s_session_last_us) * 1e-6f
                    : 0.0f;
                s_activity.duration_ms = (uint32_t)(s_session_time_s * 1000.0f);

                uint32_t stroke_delta = (ev == STROKE_EVENT_CATCH) ? 1 : 0;

                // Update Session Model (Activity.c)
                activity_update(&s_activity,
                                dt_s,
                                speed_mps,
                                spm_raw,
                                0.0f, // Power placeholder
                                dist_delta_m,
                                stroke_delta,
                                gps_ok,
                                (isfinite(s_last_valid_spm) && (t_s - s_last_spm_t_s) <= 12.0f));

                bool is_interval = activity_type_is_interval(s_activity.activity_type);
                if (is_interval && !interval_was_recording)
                {
                    interval_program_get_config(&s_interval_log_cfg);
                    s_interval_cfg_valid = true;
                    s_interval_phase_active = false;
                    s_interval_log_phase = INTERVAL_PHASE_IDLE;
                    s_interval_log_round = 0;
                }

                interval_ui_state_t ist = {0};
                bool interval_ui_valid = false;
                if (is_interval)
                {
                    uint32_t t_ms = (uint32_t)(s_session_time_s * 1000.0f);
                    interval_program_update(true, t_ms, s_activity.distance_m, s_activity.stroke_count);
                    interval_program_get_ui(&ist);
                    interval_ui_valid = true;
                    if (!s_interval_done_queued && ist.phase == INTERVAL_PHASE_DONE)
                    {
                        s_interval_done_queued = true;
                        interval_data_page_show_complete_prompt();
                        act_cmd_t done_cmd = ACT_CMD_STOP_SAVE;
                        xQueueSend(s_act_q, &done_cmd, 0);
                    }
                }

                race_ui_state_t rst = {0};
                if (s_activity.activity_type == ACTIVITY_RACE)
                {
                    race_program_update(true, s_session_time_s, s_activity.distance_m, s_activity.avg_speed_mps);
                    race_program_get_ui(&rst);
                    if (!s_interval_done_queued && rst.finished)
                    {
                        s_interval_done_queued = true;
                        act_cmd_t done_cmd = ACT_CMD_STOP_SAVE;
                        xQueueSend(s_act_q, &done_cmd, 0);
                    }
                }

                if (is_interval && interval_ui_valid)
                {
                    if (ist.phase == INTERVAL_PHASE_WORK || ist.phase == INTERVAL_PHASE_REST)
                    {
                        if (!s_interval_phase_active)
                        {
                            s_interval_phase_active = true;
                            s_interval_log_phase = ist.phase;
                            s_interval_log_round = ist.round_idx;
                            s_interval_phase_start_time_s = s_session_time_s;
                            s_interval_phase_start_dist_m = s_activity.distance_m;
                            s_interval_phase_start_strokes = s_activity.stroke_count;
                            s_phase_hr_sum = 0;
                            s_phase_hr_n = 0;
                        }
                        else if (ist.phase != s_interval_log_phase || ist.round_idx != s_interval_log_round)
                        {
                            const interval_target_t *target = NULL;
                            if (s_interval_cfg_valid)
                            {
                                target = (s_interval_log_phase == INTERVAL_PHASE_WORK) ? &s_interval_log_cfg.work : &s_interval_log_cfg.rest;
                            }
                            build_interval_row(&interval_row,
                                               s_interval_log_phase,
                                               s_interval_log_round,
                                               target,
                                               s_session_time_s - s_interval_phase_start_time_s,
                                               s_activity.distance_m - s_interval_phase_start_dist_m,
                                               s_activity.stroke_count - s_interval_phase_start_strokes);
                            interval_row.avg_hr = take_phase_hr_avg();
                            need_interval_log = true;

                            s_interval_log_phase = ist.phase;
                            s_interval_log_round = ist.round_idx;
                            s_interval_phase_start_time_s = s_session_time_s;
                            s_interval_phase_start_dist_m = s_activity.distance_m;
                            s_interval_phase_start_strokes = s_activity.stroke_count;
                        }
                    }
                    else if (s_interval_phase_active &&
                             (ist.phase == INTERVAL_PHASE_DONE || ist.phase == INTERVAL_PHASE_IDLE))
                    {
                        const interval_target_t *target = NULL;
                        if (s_interval_cfg_valid)
                        {
                            target = (s_interval_log_phase == INTERVAL_PHASE_WORK) ? &s_interval_log_cfg.work : &s_interval_log_cfg.rest;
                        }
                        build_interval_row(&interval_row,
                                           s_interval_log_phase,
                                           s_interval_log_round,
                                           target,
                                           s_session_time_s - s_interval_phase_start_time_s,
                                           s_activity.distance_m - s_interval_phase_start_dist_m,
                                           s_activity.stroke_count - s_interval_phase_start_strokes);
                        interval_row.avg_hr = take_phase_hr_avg();
                        need_interval_log = true;
                        s_interval_phase_active = false;
                        s_interval_log_phase = INTERVAL_PHASE_IDLE;
                        s_interval_log_round = 0;
                    }
                }

                // Calculate Avg Pace from Session Avg Speed
                float avg_pace_s = (s_activity.avg_speed_mps > 0.1f) ? (500.0f / s_activity.avg_speed_mps) : 0.0f;

                if (s_current_split_m > 0)
                {
                    while ((s_activity.distance_m - s_split_anchor_m) >= (float)s_current_split_m)
                    {
                        float dt_split = s_session_time_s - s_split_anchor_t;
                        float split_pace = ((float)s_current_split_m > 0.1f)
                                               ? (dt_split / ((float)s_current_split_m / 500.0f))
                                               : NAN;
                        s_last_split_delta = (isfinite(avg_pace_s) && avg_pace_s > 0.1f && isfinite(split_pace))
                                                 ? (split_pace - avg_pace_s)
                                                 : NAN;
                        s_last_split_pace = split_pace;
                        s_split_anchor_m += (float)s_current_split_m;
                        s_split_anchor_t = s_session_time_s;
                    }
                }

                // Only log on CATCH
                if (ev == STROKE_EVENT_CATCH)
                {
                    ESP_LOGI(TAG, "CATCH n=%lu spm=%.1f axis=%d a=%.2f",
                             (unsigned long)s_stroke.stroke_count,
                             (double)m.spm, s_stroke.best_axis, (double)m.a_long_f);

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
                    sensor_hub_live_t live = {0};
                    sensor_hub_get_live(&live);
                    row.hr_bpm = live.hr_bpm;
                    row.pod_valid = live.pod_valid;
                    row.pod_catch_deg = (float)live.pod_catch_ddeg / 10.0f;
                    row.pod_finish_deg = (float)live.pod_finish_ddeg / 10.0f;
                    row.pod_arc_deg = (float)live.pod_arc_ddeg / 10.0f;
                    if (s_interval_phase_active && live.hr_bpm > 0) {
                        s_phase_hr_sum += live.hr_bpm;
                        if (s_phase_hr_n < UINT16_MAX) {
                            s_phase_hr_n++;
                        }
                    }

                    need_log = true;
                }
            }
            else
            {
                if (interval_was_recording && s_interval_phase_active)
                {
                    const interval_target_t *target = NULL;
                    if (s_interval_cfg_valid)
                    {
                        target = (s_interval_log_phase == INTERVAL_PHASE_WORK) ? &s_interval_log_cfg.work : &s_interval_log_cfg.rest;
                    }
                    build_interval_row(&interval_row,
                                       s_interval_log_phase,
                                       s_interval_log_round,
                                       target,
                                       s_session_time_s - s_interval_phase_start_time_s,
                                       s_activity.distance_m - s_interval_phase_start_dist_m,
                                       s_activity.stroke_count - s_interval_phase_start_strokes);
                    interval_row.avg_hr = take_phase_hr_avg();
                    need_interval_log = true;
                    s_interval_phase_active = false;
                    s_interval_log_phase = INTERVAL_PHASE_IDLE;
                    s_interval_log_round = 0;
                }
                s_interval_cfg_valid = false;
            }

            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            if (need_log && s_log_q)
            {
                activity_log_msg_t msg = {0};
                msg.kind = ACT_LOG_ROW_STROKE;
                msg.data.stroke = row;
                xQueueSend(s_log_q, &msg, 0);
            }
            if (need_interval_log && s_log_q)
            {
                activity_log_msg_t msg = {0};
                msg.kind = ACT_LOG_ROW_INTERVAL;
                msg.data.interval = interval_row;
                xQueueSend(s_log_q, &msg, 0);
            }

            bool recording = s_activity_recording;
            s_interval_prev_recording = s_activity_recording;

            TickType_t now = xTaskGetTickCount();
            bool catch_ui = (ev == STROKE_EVENT_CATCH);
            if (!ui_is_modal_active() &&
                (catch_ui || (now - last_ui_tick) >= ui_period))
            {
                last_ui_tick = now;

                float spm_raw_ui = s_last_valid_spm;
                if (s_last_spm_t_s > 0.0f && (t_s - s_last_spm_t_s) > 12.0f)
                    spm_raw_ui = NAN;
                float spm_disp = spm_raw_ui;
                if (isfinite(spm_disp))
                    spm_disp = ceilf(spm_disp * 2.0f) / 2.0f;

                float pace = (speed_mps > 0.2f) ? (500.0f / speed_mps) : NAN;
                float avg_pace_s = (recording && s_activity.avg_speed_mps > 0.1f)
                                       ? (500.0f / s_activity.avg_speed_mps)
                                       : NAN;
                float stroke_len_disp = NAN;
                if (recording && stroke_len_m > 0.01f)
                    stroke_len_disp = stroke_len_m;

                interval_ui_state_t ist = {0};
                bool interval_active = false;
                if (recording && activity_type_is_interval(s_activity.activity_type))
                {
                    interval_program_get_ui(&ist);
                    interval_active = ist.active;
                }

                race_ui_state_t rst = {0};
                if (recording && s_activity.activity_type == ACTIVITY_RACE)
                {
                    race_program_get_ui(&rst);
                }

                uint8_t target_spm = recording ? interval_program_target_spm() : 0;

                coach_ui_snapshot_t snap = {
                    .seq = ++s_ui_seq,
                    .recording = recording,
                    .touch_locked = ui_is_touch_lock(),
                    .gps_connected = gps_connected,
                    .gps_ok = gps_ok,
                    .gps_stale = gps_connected && !gps_ok,
                    .gps_bars = gps_bars,
                    .battery_pct = 255,
                    .time_s = recording ? s_session_time_s : NAN,
                    .distance_m = recording ? s_activity.distance_m : NAN,
                    .pace_s_per_500m = recording ? pace : NAN,
                    .avg_pace_s_per_500m = recording ? avg_pace_s : NAN,
                    .speed_mps = recording ? speed_mps : NAN,
                    .spm = spm_disp,
                    .stroke_len_m = stroke_len_disp,
                    .stroke_count = recording ? s_activity.stroke_count : UINT32_MAX,
                    .activity_type = s_activity.activity_type,
                    .split_len_m = s_current_split_m,
                    .split_progress_m = recording ? (s_activity.distance_m - s_split_anchor_m) : NAN,
                    .last_split_pace_s = recording ? s_last_split_pace : NAN,
                    .last_split_delta_s = recording ? s_last_split_delta : NAN,
                    .interval_active = interval_active,
                    .interval_phase = ist.phase,
                    .interval_unit = ist.unit,
                    .interval_remaining = ist.remaining,
                    .round_idx = ist.round_idx,
                    .rounds = ist.rounds,
                    .target_spm = target_spm,
                    .race_active = rst.active,
                    .race_finished = rst.finished,
                    .race_delta_s = rst.delta_s,
                    .race_remaining_m = rst.remaining_m,
                    .race_projected_s = rst.projected_finish_s,
                };
                sensor_hub_live_t live_ui = {0};
                sensor_hub_get_live(&live_ui);
                snap.hr_bpm = live_ui.hr_bpm;
                snap.sensor_link = sensor_link_from_live(&live_ui);
                snap.pod_valid = live_ui.pod_valid;
                snap.pod_catch_deg = (float)live_ui.pod_catch_ddeg / 10.0f;
                snap.pod_finish_deg = (float)live_ui.pod_finish_ddeg / 10.0f;
                snap.pod_arc_deg = (float)live_ui.pod_arc_ddeg / 10.0f;
                coach_ui_snapshot_publish(&snap);
            }
        }
        else
        {
            static uint32_t s_imu_fail;
            s_imu_fail++;
            if ((s_imu_fail % 50u) == 1u) {
                ESP_LOGW(TAG, "IMU read failed (%lu)", (unsigned long)s_imu_fail);
                (void)i2c_helper_bus_reset(&s_imu_bus);
            }
        }

        {
            static TickType_t last_hb;
            TickType_t hb_now = xTaskGetTickCount();
            if ((hb_now - last_hb) >= pdMS_TO_TICKS(1000)) {
                last_hb = hb_now;
                ESP_LOGI("STROKE", "surge=%.2f thr=%.2f axis=%d phase=%d n=%lu spm=%.1f rec=%d",
                         (double)s_stroke.last_s0, (double)s_stroke.last_thr,
                         s_stroke.best_axis, s_stroke.phase,
                         (unsigned long)s_stroke.stroke_count,
                         (double)s_last_valid_spm, (int)s_activity_recording);
            }
        }
        xTaskDelayUntil(&last_wake, sample_delay);
    }
}
