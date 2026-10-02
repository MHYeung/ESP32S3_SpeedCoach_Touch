#include "app_activity.h"

#include "storage_paths.h"
#include "app_context.h"
#include "app_imu.h"
#include "activity.h"
#include "activity_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "interval_program.h"
#include "nvs_helper.h"
#include "race_program.h"
#include "ui.h"
#include "ui_data_page.h"
#include "ui_interval_data_page.h"
#include "ui_race_data_page.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app";

static const char *interval_unit_to_str(interval_unit_t unit)
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

static void fill_interval_log_cfg(activity_log_interval_cfg_t *out, const interval_config_t *cfg)
{
    if (!out || !cfg)
        return;
    memset(out, 0, sizeof(*out));
    out->work_value = cfg->work.value;
    strncpy(out->work_unit, interval_unit_to_str(cfg->work.unit), sizeof(out->work_unit) - 1);
    out->rest_value = cfg->rest.value;
    strncpy(out->rest_unit, interval_unit_to_str(cfg->rest.unit), sizeof(out->rest_unit) - 1);
    out->rounds = cfg->rounds;
    out->auto_advance = cfg->auto_advance;
}

void activity_worker_task(void *arg)
{
    (void)arg;

    act_cmd_t cmd;

    for (;;)
    {
        if (xQueueReceive(s_act_q, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        if (s_activity_mutex)
            xSemaphoreTake(s_activity_mutex, portMAX_DELAY);

        if (cmd == ACT_CMD_START)
        {
            s_activity_recording = true;
            s_session_time_s = 0.0f;
            s_session_last_us = esp_timer_get_time();
            s_interval_done_queued = false;
            app_imu_request_session_reset();
            if (nvs_helper_get_metrics_lock()) {
                ui_set_touch_lock(true);
            }

            uint32_t id = nvs_helper_get_next_activity_id();
            activity_init(&s_activity, id);
            activity_start(&s_activity, time(NULL));
            activity_set_type(&s_activity, ACTIVITY_NORMAL);

            activity_log_set_interval_config(&s_act_log, NULL);
            activity_log_set_split_interval(&s_act_log, s_current_split_m);
            s_last_session_stroke_count = 0;

            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            ui_set_interval_data_visible(false);
            ui_set_race_data_visible(false);
            if (s_sd.mounted)
                activity_log_start(&s_act_log, &s_sd, s_activity.start_ts, id, ACTIVITY_NORMAL);

            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START id=%lu", (unsigned long)id);
            continue;
        }

        if (cmd == ACT_CMD_START_INTERVAL_NORMAL)
        {
            s_activity_recording = true;
            s_session_time_s = 0.0f;
            s_session_last_us = esp_timer_get_time();
            s_interval_done_queued = false;
            app_imu_request_session_reset();
            if (nvs_helper_get_metrics_lock()) {
                ui_set_touch_lock(true);
            }

            uint32_t id = nvs_helper_get_next_activity_id();
            activity_init(&s_activity, id);
            activity_start(&s_activity, time(NULL));
            activity_set_type(&s_activity, ACTIVITY_INTERVAL_NORMAL);
            (void)resolve_interval_split_m();
            activity_log_set_split_interval(&s_act_log, s_current_split_m);

            interval_config_t cfg = {0};
            interval_program_get_config(&cfg);
            activity_log_interval_cfg_t log_cfg = {0};
            fill_interval_log_cfg(&log_cfg, &cfg);
            activity_log_set_interval_config(&s_act_log, &log_cfg);
            s_last_session_stroke_count = 0;

            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            ui_set_interval_data_visible(true);
            interval_data_page_hide_start_prompt();
            ui_set_race_data_visible(false);
            ui_go_to_page(UI_INTERVAL_DATA_PAGE, false);
            if (s_sd.mounted)
                activity_log_start(&s_act_log, &s_sd, s_activity.start_ts, id, ACTIVITY_INTERVAL_NORMAL);
            interval_program_start();
            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START_INTERVAL id=%lu", (unsigned long)id);
            continue;
        }

        if (cmd == ACT_CMD_START_INTERVAL_STEP)
        {
            s_activity_recording = true;
            s_session_time_s = 0.0f;
            s_session_last_us = esp_timer_get_time();
            s_interval_done_queued = false;
            app_imu_request_session_reset();
            if (nvs_helper_get_metrics_lock()) {
                ui_set_touch_lock(true);
            }

            uint32_t id = nvs_helper_get_next_activity_id();
            activity_init(&s_activity, id);
            activity_start(&s_activity, time(NULL));
            activity_set_type(&s_activity, ACTIVITY_INTERVAL_STEP);
            (void)resolve_interval_split_m();
            activity_log_set_split_interval(&s_act_log, s_current_split_m);

            interval_config_t cfg = {0};
            interval_program_get_config(&cfg);
            activity_log_interval_cfg_t log_cfg = {0};
            fill_interval_log_cfg(&log_cfg, &cfg);
            activity_log_set_interval_config(&s_act_log, &log_cfg);
            s_last_session_stroke_count = 0;

            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            ui_set_interval_data_visible(true);
            interval_data_page_hide_start_prompt();
            ui_go_to_page(UI_INTERVAL_DATA_PAGE, false);
            if (s_sd.mounted)
                activity_log_start(&s_act_log, &s_sd, s_activity.start_ts, id, ACTIVITY_INTERVAL_STEP);
            interval_program_start();
            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START_STEP id=%lu", (unsigned long)id);
            continue;
        }

        if (cmd == ACT_CMD_START_RACE)
        {
            s_activity_recording = true;
            s_session_time_s = 0.0f;
            s_session_last_us = esp_timer_get_time();
            s_interval_done_queued = false;
            app_imu_request_session_reset();
            if (nvs_helper_get_metrics_lock()) {
                ui_set_touch_lock(true);
            }

            uint32_t id = nvs_helper_get_next_activity_id();
            activity_init(&s_activity, id);
            activity_start(&s_activity, time(NULL));
            activity_set_type(&s_activity, ACTIVITY_RACE);
            activity_log_set_interval_config(&s_act_log, NULL);
            activity_log_set_split_interval(&s_act_log, s_current_split_m);
            s_last_session_stroke_count = 0;

            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            ui_set_interval_data_visible(false);
            ui_set_race_data_visible(true);
            race_data_page_hide_start_prompt();
            ui_go_to_page(UI_RACE_DATA_PAGE, false);
            if (s_sd.mounted)
                activity_log_start(&s_act_log, &s_sd, s_activity.start_ts, id, ACTIVITY_RACE);
            race_program_start();
            data_page_show_activity_toast(true);
            ESP_LOGI("ACT", "START_RACE id=%lu", (unsigned long)id);
            continue;
        }

        if (cmd == ACT_CMD_STOP_SAVE)
        {
            s_activity_recording = false;
            ui_set_interval_data_visible(false);
            ui_set_race_data_visible(false);
            interval_data_page_hide_start_prompt();
            race_data_page_hide_start_prompt();
            if (ui_get_current_page() == UI_RACE_DATA_PAGE)
                ui_go_to_page(UI_PAGE_DATA, false);
            interval_program_stop();
            race_program_stop();
            s_interval_done_queued = false;
            ui_set_touch_lock(false);

            if (s_session_last_us > 0) {
                int64_t elapsed_us = esp_timer_get_time() - s_session_last_us;
                if (elapsed_us < 0)
                    elapsed_us = 0;
                uint32_t dur_ms = (uint32_t)(elapsed_us / 1000);
                if (dur_ms > s_activity.duration_ms)
                    s_activity.duration_ms = dur_ms;
            }

            activity_stop(&s_activity, time(NULL));

            activity_t snapshot = s_activity;
            if (s_activity_mutex)
                xSemaphoreGive(s_activity_mutex);

            data_page_show_activity_toast(false);
            ESP_LOGI("ACT", "STOP id=%lu Dist=%.1fm dur=%.1fs",
                     (unsigned long)snapshot.id, (double)snapshot.distance_m,
                     (double)(snapshot.duration_ms / 1000.0f));

            float final_time_s = snapshot.duration_ms / 1000.0f;
            if (s_act_log.opened)
            {
                /* Drain IMU log messages so last GPS/stroke distance is in the file
                 * before we write the remainder split. */
                activity_log_msg_t pending;
                while (s_log_q && xQueueReceive(s_log_q, &pending, 0) == pdTRUE)
                {
                    if (pending.kind == ACT_LOG_ROW_STROKE)
                        activity_log_append(&s_act_log, &pending.data.stroke);
                    else if (pending.kind == ACT_LOG_ROW_INTERVAL)
                        activity_log_append_interval(&s_act_log, &pending.data.interval);
                }

                float dist_m = snapshot.distance_m;
                if (s_act_log.last_row_distance_m > dist_m)
                    dist_m = s_act_log.last_row_distance_m;

                bool need_final_row = !s_act_log.has_last_row ||
                                      (dist_m - s_act_log.last_row_distance_m) >= 0.1f ||
                                      (final_time_s - s_act_log.last_row_time_s) >= 0.1f;

                if (need_final_row)
                {
                    activity_log_row_t final_row = {0};
                    float avg_pace_s = (snapshot.avg_speed_mps > 0.1f) ? (500.0f / snapshot.avg_speed_mps) : 0.0f;

                    final_row.rtc_time = snapshot.end_ts;
                    final_row.session_time_s = final_time_s;
                    final_row.total_distance_m = dist_m;
                    final_row.pace_500m_s = avg_pace_s;
                    final_row.spm_instant = snapshot.avg_spm;
                    final_row.avg_pace_500m_s = avg_pace_s;
                    final_row.avg_speed_mps = snapshot.avg_speed_mps;
                    final_row.stroke_length_m = 0.0f;
                    final_row.stroke_count = snapshot.stroke_count;
                    final_row.gps_lat = 0.0;
                    final_row.gps_lon = 0.0;
                    final_row.power_w = 0.0f;
                    final_row.drive_time_s = 0.0f;
                    final_row.recovery_time_s = 0.0f;
                    final_row.recovery_ratio = 0.0f;

                    activity_log_append(&s_act_log, &final_row);
                }

                activity_log_finalize(&s_act_log, dist_m, final_time_s, snapshot.avg_spm);
            }

            // STOP THE LOGGER: This flushes and closes the CSV file.
            // The file is now complete and saved on the SD card.
            activity_log_stop(&s_act_log);

            FILE *f = fopen(ACTIVITY_INDEX_PATH, "a");
            if (f)
            {
                uint32_t duration_s = (uint32_t)(snapshot.duration_ms / 1000);
                float avg_pace_s_per500 = 0.0f;
                if (snapshot.avg_speed_mps > 0.01f)
                {
                    avg_pace_s_per500 = 500.0f / snapshot.avg_speed_mps; // seconds per 500m
                }

                // IMPORTANT: store the base path (no _Splits/_Strokes suffix)
                // In your activity_log.c you build: "activities/<base_name>"
                // Use the field you actually have in activity_log_t:
                //   - if it's log->filename_base use that
                //   - if it's log->rel_path rename accordingly
                fprintf(f, "%lu,%ld,%lu,%.2f,%.2f,%s\n",
                        (unsigned long)snapshot.id,
                        (long)snapshot.start_ts,
                        (unsigned long)duration_s,
                        (double)snapshot.distance_m,
                        (double)avg_pace_s_per500,
                        s_act_log.filename_base /* or s_act_log.rel_path */);

                fclose(f);
            }
        }
    }
}

esp_err_t app_activity_post_cmd(act_cmd_t cmd)
{
    if (!s_act_q)
        return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_act_q, &cmd, 0) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

void on_stop_save_confirmed(void)
{
    (void)app_activity_post_cmd(ACT_CMD_STOP_SAVE);
}

void activity_logger_task(void *arg)
{
    (void)arg;

    activity_log_msg_t msg;
    for (;;)
    {
        if (xQueueReceive(s_log_q, &msg, portMAX_DELAY) == pdTRUE)
        {
            // Only append if file is open
            if (s_act_log.opened)
            {
                if (msg.kind == ACT_LOG_ROW_STROKE)
                {
                    activity_log_append(&s_act_log, &msg.data.stroke);
                }
                else if (msg.kind == ACT_LOG_ROW_INTERVAL)
                {
                    activity_log_append_interval(&s_act_log, &msg.data.interval);
                }
            }
        }
    }
}

void on_split_interval_changed(uint32_t length_m)
{
    ESP_LOGI(TAG, "UI Callback: Split Interval changed to %lu meters", length_m);

    // Update the logger configuration immediately
    s_current_split_m = length_m;
    activity_log_set_split_interval(&s_act_log, length_m);
}

uint32_t resolve_interval_split_m(void)
{
    interval_config_t cfg = {0};
    interval_program_get_config(&cfg);

    if (cfg.work.unit == INTERVAL_UNIT_DISTANCE && cfg.work.value > 0)
        return cfg.work.value;

    return s_current_split_m;
}
