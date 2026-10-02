#include "activity_log.h"
#include "sensor_hub.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "math.h"
#include "esp_log.h"

static const char *TAG = "activity_log";

/* -------------------------------------------------------------------------- */
/* Format Helpers (Preserved)                                                */
/* -------------------------------------------------------------------------- */

// Convert time_t to "YYYY-MM-DD HH:MM:SS"
static void format_timestamp(time_t ts, char *buf, size_t len)
{
    if (!buf || len < 20)
        return;
    struct tm tm_info;
    localtime_r(&ts, &tm_info);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

const char* get_activity_string_fast(activity_type_t type) {
    // defined purely for clarity; ideally usually placed in a header or static scope
    static const char* const activity_map[] = {
        [ACTIVITY_NORMAL]          = "Normal",
        [ACTIVITY_INTERVAL_NORMAL] = "Intervals - Rounds",
        [ACTIVITY_INTERVAL_STEP]   = "Intervals - Steps",
        [ACTIVITY_RACE]            = "Race"
    };

    // Safety check to prevent crashing on invalid input
    if (type < 0 || type >= (sizeof(activity_map) / sizeof(activity_map[0]))) {
        return "Unknown";
    }

    return activity_map[type];
}

static void fmt_session_time_ms(float total_sec, char *out, size_t len)
{
    if (total_sec < 0)
        total_sec = 0;

    int h = (int)(total_sec / 3600);
    int rem = (int)total_sec % 3600;
    int m = rem / 60;
    int s = rem % 60;
    int ms = (int)((total_sec - floorf(total_sec)) * 1000.0f);

    if (ms < 0)
        ms = 0;
    if (ms > 999)
        ms = 999;

    snprintf(out, len, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

// Convert seconds float to "MM:SS.s" (Pace format)
static void format_pace(float seconds, char *buf, size_t len)
{
    if (!buf || len == 0)
        return;

    if (seconds <= 0.0f || seconds > 3600.0f)
    {
        snprintf(buf, len, "--:--.-");
        return;
    }

    int min = (int)(seconds / 60.0f);
    float sec_rem = seconds - (min * 60.0f);

    snprintf(buf, len, "%02d:%04.1f", min, sec_rem);
}

static void activity_log_update_splits(activity_log_t *log,
                                       float total_distance_m,
                                       float session_time_s,
                                       float spm_value,
                                       bool include_partial)
{
    if (!log || !log->opened || !log->f_splits)
        return;
    if (log->split_interval_m <= 0.0f)
        return;
    if (total_distance_m < log->last_split_dist_m || session_time_s < log->last_split_time_s)
        return;

    uint16_t avg_hr = (log->hr_n > 0) ? (uint16_t)(log->hr_sum / log->hr_n) : 0;
    bool wrote_split = false;

    float dist_delta = total_distance_m - log->last_split_dist_m;
    float time_delta = session_time_s - log->last_split_time_s;

    while (dist_delta >= log->split_interval_m && log->split_interval_m > 0.0f)
    {
        float ratio = (dist_delta > 0.0f) ? (log->split_interval_m / dist_delta) : 0.0f;
        float exact_split_time = time_delta * ratio;
        float split_total_dist = log->last_split_dist_m + log->split_interval_m;
        float split_pace_s = (log->split_interval_m > 0.1f)
                                 ? (exact_split_time / (log->split_interval_m / 500.0f))
                                 : 0.0f;

        activity_log_split_row_t split = {
            .split_index = log->next_split_index++,
            .total_dist_m = split_total_dist,
            .split_dist_m = log->split_interval_m,
            .split_time_s = exact_split_time,
            .split_pace_s = split_pace_s,
            .avg_spm = spm_value,
            .avg_hr = avg_hr};

        activity_log_append_split(log, &split);
        wrote_split = true;

        log->last_split_dist_m = split_total_dist;
        log->last_split_time_s += exact_split_time;

        dist_delta = total_distance_m - log->last_split_dist_m;
        time_delta = session_time_s - log->last_split_time_s;
    }

    if (include_partial)
    {
        /* Incomplete last split still gets a row: 490 m of a 500 m setting,
         * or a timed piece with no GPS distance yet. */
        if (time_delta >= 0.5f)
        {
            if (dist_delta < 0.0f)
                dist_delta = 0.0f;

            float split_pace_s = (dist_delta > 0.1f)
                                     ? (time_delta / (dist_delta / 500.0f))
                                     : 0.0f;

            activity_log_split_row_t split = {
                .split_index = log->next_split_index++,
                .total_dist_m = total_distance_m,
                .split_dist_m = dist_delta,
                .split_time_s = time_delta,
                .split_pace_s = split_pace_s,
                .avg_spm = spm_value,
                .avg_hr = avg_hr};

            activity_log_append_split(log, &split);
            wrote_split = true;
            ESP_LOGI(TAG, "Remainder split #%d dist=%.1fm time=%.1fs",
                     split.split_index, (double)dist_delta, (double)time_delta);

            log->last_split_dist_m = total_distance_m;
            log->last_split_time_s = session_time_s;
        }
    }

    if (wrote_split) {
        log->hr_sum = 0;
        log->hr_n = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* File / Dir Helpers                                                        */
/* -------------------------------------------------------------------------- */

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    return (mkdir(path, 0775) == 0) ? ESP_OK : ESP_FAIL;
}

// Modified to generate base name without extension, so we can append .csv and _Splits.csv
static void build_filename_base(time_t start_ts, uint32_t id, char *out, size_t out_len)
{
    struct tm tm_local;
    localtime_r(&start_ts, &tm_local);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d_%02u",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, (unsigned)(id % 100));
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                */
/* -------------------------------------------------------------------------- */

void activity_log_init(activity_log_t *log)
{
    if (!log)
        return;
    memset(log, 0, sizeof(activity_log_t));
    log->flush_every_n = 5;
}

void activity_log_set_split_interval(activity_log_t *log, uint32_t interval_m)
{
    if (log)
    {
        log->split_interval_m = (float)interval_m;
        ESP_LOGI(TAG, "Split interval set to %.0fm", log->split_interval_m);
    }
}

esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_ts, uint32_t activity_id, activity_type_t act_type)
{
    if (!log || !sd || !sd->mounted)
        return ESP_ERR_INVALID_STATE;

    float cached_interval = log->split_interval_m;
    activity_log_interval_cfg_t cached_interval_cfg = log->interval_cfg;
    bool cached_interval_cfg_valid = log->interval_cfg_valid;
    bool is_interval = activity_type_is_interval(act_type);

    activity_log_init(log);

    log->split_interval_m = (cached_interval > 0.1f) ? cached_interval : 1000.0f;
    log->interval_cfg = cached_interval_cfg;
    log->interval_cfg_valid = cached_interval_cfg_valid;
    log->next_split_index = 1;
    log->activity_type = act_type;

    // 1. Create Directory
    char dir_full[128];
    snprintf(dir_full, sizeof(dir_full), "%s/activities", sd->mount_point);
    ensure_dir(dir_full);

    // 2. Generate Base Name (activities/YYYYMMDD...)
    char base_name[64];
    build_filename_base(start_ts, activity_id, base_name, sizeof(base_name));

    // 3. Create per-activity directory
    char act_dir[160];
    snprintf(act_dir, sizeof(act_dir), "%s/activities/%s", sd->mount_point, base_name);
    ensure_dir(act_dir);

    // Store relative path base for reference (used by index.csv)
    snprintf(log->filename_base, sizeof(log->filename_base), "activities/%s", base_name);

    // 4. Open Main Log File (.csv)
    char full_path_main[180];
    snprintf(full_path_main, sizeof(full_path_main), "%s/activities/%s/Strokes.csv",
             sd->mount_point, base_name);

    log->f_main = fopen(full_path_main, "w");
    if (!log->f_main)
    {
        ESP_LOGE(TAG, "fopen main failed: %s", full_path_main);
        return ESP_FAIL;
    }

    // 5. Open Splits Log File (_Splits.csv)
    char full_path_splits[180];
    snprintf(full_path_splits, sizeof(full_path_splits), "%s/activities/%s/Splits.csv",
             sd->mount_point, base_name);

    log->f_splits = fopen(full_path_splits, "w");
    if (!log->f_splits)
    {
        ESP_LOGW(TAG, "fopen splits failed: %s", full_path_splits);
        // We can continue with just the main log if splits fail
    }

    // 5. Write Headers
    if (log->f_main)
    {
        // Your Custom Header
        fprintf(log->f_main,
                "Global Time,Session Time,Distance (m),Pace (/500m),SPM,Avg Pace (/500m),Average Speed (m/s),"
                "Stroke Length (m),Stroke Count,gps_lat,gps_lon,Power (W),Drive Time (s),Recovery Time (s),Recovery Ratio,"
                "Heart Rate (bpm),Catch (deg),Finish (deg),Arc (deg)\n");
    }

    if (log->f_splits)
    {
        // 1. Prepare Metadata Strings
        char time_str[32];
        format_timestamp(start_ts, time_str, sizeof(time_str)); // Uses your existing helper

        // 2. Write Metadata Rows (Device Settings)
        fprintf(log->f_splits, "Device Info,ESP32-S3 Speed Coach\n");
        fprintf(log->f_splits, "Session Start,%s\n", time_str);
        fprintf(log->f_splits, "Activity Type,%s\n",  get_activity_string_fast(log->activity_type));
        if (is_interval)
        {
            if (log->interval_cfg_valid)
            {
                fprintf(log->f_splits, "Interval Work,%u %s\n",
                        (unsigned)log->interval_cfg.work_value,
                        log->interval_cfg.work_unit);
                fprintf(log->f_splits, "Interval Rest,%u %s\n",
                        (unsigned)log->interval_cfg.rest_value,
                        log->interval_cfg.rest_unit);
                fprintf(log->f_splits, "Rounds,%u\n", (unsigned)log->interval_cfg.rounds);
                fprintf(log->f_splits, "Auto Advance,%u\n", log->interval_cfg.auto_advance ? 1U : 0U);
            }
        }
        else
        {
            fprintf(log->f_splits, "Split Setting,%.0f meters\n", log->split_interval_m);
        }
        fprintf(log->f_splits, "Activity ID,%u\n", (unsigned int)activity_id);
        {
            sensor_hub_slot_info_t hr = {0};
            sensor_hub_get_slot(SENSOR_KIND_HR, &hr);
            fprintf(log->f_splits, "HR Sensor,%s\n",
                    (hr.saved && hr.name[0]) ? hr.name : "none");
        }

        // 3. Add an empty row for separation (optional but readable)
        fprintf(log->f_splits, "\n");

        // 4. Write the Actual Data Columns
        if (is_interval)
        {
            fprintf(log->f_splits, "Round,Phase,Target,Unit,Phase Time,Phase Dist (m),Phase Pace (/500m),Avg SPM,Avg HR\n");
        }
        else
        {
            fprintf(log->f_splits, "Split #,Total Dist (m),Split Dist (m),Split Time,Avg Pace (/500m),Avg SPM,Avg HR\n");
        }
    }

    log->opened = true;
    ESP_LOGI(TAG, "Started Activity: %s", base_name);
    return ESP_OK;
}

esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row)
{
    if (!log || !log->opened || !log->f_main)
        return ESP_ERR_INVALID_STATE;

    // --- 1. Write Stroke Row ---
    char time_str[32];
    format_timestamp(row->rtc_time, time_str, sizeof(time_str));
    char pace_inst_str[24];
    format_pace(row->pace_500m_s, pace_inst_str, sizeof(pace_inst_str));
    char pace_avg_str[24];
    format_pace(row->avg_pace_500m_s, pace_avg_str, sizeof(pace_avg_str));
    char session_time_str[32];
    fmt_session_time_ms(row->session_time_s, session_time_str, sizeof(session_time_str));

    char hr_str[8] = {0};
    char catch_str[16] = {0};
    char finish_str[16] = {0};
    char arc_str[16] = {0};
    if (row->hr_bpm > 0) {
        snprintf(hr_str, sizeof(hr_str), "%u", (unsigned)row->hr_bpm);
    }
    if (row->pod_valid) {
        snprintf(catch_str, sizeof(catch_str), "%.1f", (double)row->pod_catch_deg);
        snprintf(finish_str, sizeof(finish_str), "%.1f", (double)row->pod_finish_deg);
        snprintf(arc_str, sizeof(arc_str), "%.1f", (double)row->pod_arc_deg);
    }
    fprintf(log->f_main, "%s,%s,%.1f,%s,%.1f,%s,%.2f,%.2f,%lu,%.7f,%.7f,%.1f,%.2f,%.2f,%.2f,%s,%s,%s,%s\n",
            time_str, session_time_str, (double)row->total_distance_m, pace_inst_str,
            (double)row->spm_instant, pace_avg_str, (double)row->avg_speed_mps,
            (double)row->stroke_length_m, (unsigned long)row->stroke_count,
            row->gps_lat, row->gps_lon, (double)row->power_w,
            (double)row->drive_time_s, (double)row->recovery_time_s, (double)row->recovery_ratio,
            hr_str, catch_str, finish_str, arc_str);

    if (row->hr_bpm > 0 && log->hr_n < UINT16_MAX) {
        log->hr_sum += row->hr_bpm;
        log->hr_n++;
    }

    log->pending++;
    if (log->pending >= log->flush_every_n)
    {
        fflush(log->f_main);
        log->pending = 0;
    }

    log->has_last_row = true;
    log->last_row_distance_m = row->total_distance_m;
    log->last_row_time_s = row->session_time_s;
    log->last_row_stroke_count = row->stroke_count;

    if (!activity_type_is_interval(log->activity_type))
    {
        activity_log_update_splits(log,
                                   row->total_distance_m,
                                   row->session_time_s,
                                   row->spm_instant,
                                   false);
    }

    return ESP_OK;
}

esp_err_t activity_log_append_split(activity_log_t *log, const activity_log_split_row_t *row)
{
    if (!log || !log->opened || !log->f_splits)
        return ESP_ERR_INVALID_STATE;

    char split_time_str[32];
    fmt_session_time_ms(row->split_time_s, split_time_str, sizeof(split_time_str));

    char pace_str[24];
    format_pace(row->split_pace_s, pace_str, sizeof(pace_str));

    char hr_str[8] = {0};
    if (row->avg_hr > 0) {
        snprintf(hr_str, sizeof(hr_str), "%u", (unsigned)row->avg_hr);
    }
    fprintf(log->f_splits, "%d,%.0f,%.0f,%s,%s,%.1f,%s\n",
            row->split_index,
            (double)row->total_dist_m,
            (double)row->split_dist_m,
            split_time_str,
            pace_str,
            (double)row->avg_spm,
            hr_str);

    fflush(log->f_splits);
    return ESP_OK;
}

void activity_log_set_interval_config(activity_log_t *log, const activity_log_interval_cfg_t *cfg)
{
    if (!log)
        return;
    if (!cfg)
    {
        log->interval_cfg_valid = false;
        memset(&log->interval_cfg, 0, sizeof(log->interval_cfg));
        return;
    }
    log->interval_cfg = *cfg;
    log->interval_cfg_valid = true;
}

esp_err_t activity_log_append_interval(activity_log_t *log, const activity_log_interval_row_t *row)
{
    if (!log || !log->opened || !log->f_splits)
        return ESP_ERR_INVALID_STATE;
    if (!row)
        return ESP_ERR_INVALID_ARG;

    char phase_time_str[32];
    fmt_session_time_ms(row->phase_time_s, phase_time_str, sizeof(phase_time_str));

    char pace_str[24];
    format_pace(row->phase_pace_s, pace_str, sizeof(pace_str));

    char hr_str[8] = {0};
    if (row->avg_hr > 0) {
        snprintf(hr_str, sizeof(hr_str), "%u", (unsigned)row->avg_hr);
    }
    fprintf(log->f_splits, "%u,%s,%u,%s,%s,%.1f,%s,%.1f,%s\n",
            (unsigned)row->round_index,
            (row->phase[0] != 0) ? row->phase : "--",
            (unsigned)row->target_value,
            (row->target_unit[0] != 0) ? row->target_unit : "--",
            phase_time_str,
            (double)row->phase_distance_m,
            pace_str,
            (double)row->avg_spm,
            hr_str);

    fflush(log->f_splits);
    return ESP_OK;
}

esp_err_t activity_log_finalize(activity_log_t *log, float final_distance_m, float final_time_s, float avg_spm)
{
    if (!log || !log->opened)
        return ESP_ERR_INVALID_STATE;

    if (!activity_type_is_interval(log->activity_type))
    {
        activity_log_update_splits(log, final_distance_m, final_time_s, avg_spm, true);
    }
    return ESP_OK;
}

esp_err_t activity_log_stop(activity_log_t *log)
{
    if (log->opened)
    {
        if (log->f_main)
        {
            fflush(log->f_main);
            fclose(log->f_main);
            log->f_main = NULL;
        }
        if (log->f_splits)
        {
            fflush(log->f_splits);
            fclose(log->f_splits);
            log->f_splits = NULL;
        }
        log->opened = false;
    }
    return ESP_OK;
}
