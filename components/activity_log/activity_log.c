#include "activity_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>        // Required for localtime_r, strftime
#include "esp_log.h"

static const char *TAG = "activity_log";

/* -------------------------------------------------------------------------- */
/* Format Helpers                                                            */
/* -------------------------------------------------------------------------- */

// Convert time_t to "YYYY-MM-DD HH:MM:SS"
static void format_timestamp(time_t ts, char *buf, size_t len)
{
    if (!buf || len < 20) return;
    struct tm tm_info;
    localtime_r(&ts, &tm_info); // Converts epoch to local struct tm
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

// Convert seconds float to "MM:SS.s /500m"
// Example: 133.2 seconds -> "02:13.2 /500m"
static void format_pace(float seconds, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    
    // Safety for 0 or invalid pace
    if (seconds <= 0.0f || seconds > 3600.0f) {
        snprintf(buf, len, "--:--.- /500m");
        return;
    }

    int min = (int)(seconds / 60.0f);
    float sec_rem = seconds - (min * 60.0f);

    snprintf(buf, len, "%02d:%04.1f /500m", min, sec_rem);
}

/* -------------------------------------------------------------------------- */
/* File / Dir Helpers                                                        */
/* -------------------------------------------------------------------------- */

static esp_err_t ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    return (mkdir(path, 0775) == 0) ? ESP_OK : ESP_FAIL;
}

static void build_csv_name(time_t start_ts, uint32_t id, char *out, size_t out_len) {
    struct tm tm_local;
    localtime_r(&start_ts, &tm_local);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d_%02u.csv",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, (unsigned)(id % 100));
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_ts, uint32_t activity_id)
{
    if (!log || !sd || !sd->mounted) return ESP_ERR_INVALID_STATE;

    memset(log, 0, sizeof(*log));
    log->flush_every_n = 5; 

    char dir_full[128];
    snprintf(dir_full, sizeof(dir_full), "%s/activities", sd->mount_point);
    ensure_dir(dir_full);

    char name[64];
    build_csv_name(start_ts, activity_id, name, sizeof(name));
    snprintf(log->rel_path, sizeof(log->rel_path), "activities/%s", name);

    char full_path[160];
    snprintf(full_path, sizeof(full_path), "%s/%s", sd->mount_point, log->rel_path);

    log->fp = fopen(full_path, "w");
    if (!log->fp) {
        ESP_LOGE(TAG, "fopen failed: %s", full_path);
        return ESP_FAIL;
    }

    // Header matches your requirements
    fprintf(log->fp, 
        "rtc_time,session_time_s,distance_m,pace_500m,spm_inst,avg_pace_500m,avg_speed_mps,"
        "stroke_len_m,stroke_count,gps_lat,gps_lon,power_w,drive_s,recov_s,recov_ratio\n");

    log->opened = true;
    return ESP_OK;
}

esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row)
{
    if (!log || !log->opened || !log->fp) return ESP_ERR_INVALID_STATE;

    // 1. Format RTC Time
    char time_str[32];
    format_timestamp(row->rtc_time, time_str, sizeof(time_str));

    // 2. Format Instant Pace
    char pace_inst_str[24];
    format_pace(row->pace_500m_s, pace_inst_str, sizeof(pace_inst_str));

    // 3. Format Average Pace
    char pace_avg_str[24];
    format_pace(row->avg_pace_500m_s, pace_avg_str, sizeof(pace_avg_str));

    // 4. Write CSV row using formatted strings for time/pace
    // Note: We use %s for the pre-formatted buffers
    fprintf(log->fp, "%s,%.2f,%.1f,%s,%.1f,%s,%.2f,%.2f,%lu,%.7f,%.7f,%.1f,%.2f,%.2f,%.2f\n",
        time_str,                       // 1. rtc_time (formatted)
        (double)row->session_time_s,    // 2. session_time
        (double)row->total_distance_m,  // 3. distance
        pace_inst_str,                  // 4. pace (formatted)
        (double)row->spm_instant,       // 5. spm
        pace_avg_str,                   // 6. avg pace (formatted)
        (double)row->avg_speed_mps,     // 7. avg speed
        (double)row->stroke_length_m,   // 8. stroke len
        (unsigned long)row->stroke_count, // 9. count
        row->gps_lat,                   // 10. lat
        row->gps_lon,                   // 11. lon
        (double)row->power_w,           // 12. power
        (double)row->drive_time_s,      // 13. drive
        (double)row->recovery_time_s,   // 14. recovery
        (double)row->recovery_ratio     // 15. ratio
    );

    log->pending++;
    if (log->pending >= log->flush_every_n) {
        fflush(log->fp);
        log->pending = 0;
    }
    return ESP_OK;
}

esp_err_t activity_log_stop(activity_log_t *log)
{
    if (log && log->fp) {
        fflush(log->fp);
        fclose(log->fp);
        log->fp = NULL;
        log->opened = false;
    }
    return ESP_OK;
}