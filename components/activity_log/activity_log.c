#include "activity_log.h"
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

esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_ts, uint32_t activity_id)
{
    if (!log || !sd || !sd->mounted)
        return ESP_ERR_INVALID_STATE;

    float cached_interval = log->split_interval_m; 

    activity_log_init(log); 

    log->split_interval_m = (cached_interval > 0.1f) ? cached_interval : 1000.0f;
    log->next_split_index = 1;

    // 1. Create Directory
    char dir_full[128];
    snprintf(dir_full, sizeof(dir_full), "%s/activities", sd->mount_point);
    ensure_dir(dir_full);

    // 2. Generate Base Name (activities/YYYYMMDD...)
    char base_name[64];
    build_filename_base(start_ts, activity_id, base_name, sizeof(base_name));

    // Store relative path base for reference
    snprintf(log->filename_base, sizeof(log->filename_base), "activities/%s", base_name);

    // 3. Open Main Log File (.csv)
    char full_path_main[160];
    snprintf(full_path_main, sizeof(full_path_main), "%s/activities/%s_Strokes.csv", sd->mount_point, base_name);

    log->f_main = fopen(full_path_main, "w");
    if (!log->f_main)
    {
        ESP_LOGE(TAG, "fopen main failed: %s", full_path_main);
        return ESP_FAIL;
    }

    // 4. Open Splits Log File (_Splits.csv)
    char full_path_splits[160];
    snprintf(full_path_splits, sizeof(full_path_splits), "%s/activities/%s_Splits.csv", sd->mount_point, base_name);

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
                "Stroke Length (m),Stroke Count,gps_lat,gps_lon,Power (W),Drive Time (s),Recovery Time (s),Recovery Ratio\n");
    }

    if (log->f_splits) {
        // 1. Prepare Metadata Strings
        char time_str[32];
        format_timestamp(start_ts, time_str, sizeof(time_str)); // Uses your existing helper
        
        // 2. Write Metadata Rows (Device Settings)
        fprintf(log->f_splits, "Device Info,ESP32S3-BLE Rowing Speed Coach\n");
        fprintf(log->f_splits, "Session Start,%s\n", time_str);
        fprintf(log->f_splits, "Split Setting,%.0f meters\n", log->split_interval_m); //
        fprintf(log->f_splits, "Activity ID,%u\n", (unsigned int)activity_id);
        
        // 3. Add an empty row for separation (optional but readable)
        fprintf(log->f_splits, "\n");

        // 4. Write the Actual Data Columns
        fprintf(log->f_splits, "Split #,Total Dist (m),Split Dist (m),Split Time,Avg Pace (/500m),Avg SPM\n");
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

    fprintf(log->f_main, "%s,%s,%.1f,%s,%.1f,%s,%.2f,%.2f,%lu,%.7f,%.7f,%.1f,%.2f,%.2f,%.2f\n",
            time_str, session_time_str, (double)row->total_distance_m, pace_inst_str,
            (double)row->spm_instant, pace_avg_str, (double)row->avg_speed_mps,
            (double)row->stroke_length_m, (unsigned long)row->stroke_count,
            row->gps_lat, row->gps_lon, (double)row->power_w,
            (double)row->drive_time_s, (double)row->recovery_time_s, (double)row->recovery_ratio);

    log->pending++;
    if (log->pending >= log->flush_every_n)
    {
        fflush(log->f_main);
        log->pending = 0;
    }

    // --- 2. Check for Split ---
    // If enabled (>0) and we crossed the threshold relative to last split
    if (log->split_interval_m > 0)
    {
        float dist_delta = row->total_distance_m - log->last_split_dist_m;

        if (dist_delta >= log->split_interval_m)
        {
            float time_delta = row->session_time_s - log->last_split_time_s;

            // Avoid division by zero
            float split_pace = 0;
            if (dist_delta > 0)
            {
                // Pace = Time / (Dist / 500)
                split_pace = time_delta / (dist_delta / 500.0f);
            }

            // Create Split Row
            activity_log_split_row_t split = {
                .split_index = log->next_split_index++,
                .total_dist_m = row->total_distance_m,
                .split_dist_m = dist_delta,
                .split_time_s = time_delta,
                .split_pace_s = split_pace,
                .avg_spm = row->spm_instant // Simplified: Current SPM is proxy for Avg SPM of split
                                            // (Ideally caller calculates true avg, but this suffices for dummy data)
            };

            activity_log_append_split(log, &split);

            // Update State
            log->last_split_dist_m = row->total_distance_m;
            log->last_split_time_s = row->session_time_s;
        }
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

    fprintf(log->f_splits, "%d,%.0f,%.0f,%s,%s,%.1f\n",
            row->split_index,
            (double)row->total_dist_m,
            (double)row->split_dist_m,
            split_time_str,
            pace_str,
            (double)row->avg_spm);

    fflush(log->f_splits);
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