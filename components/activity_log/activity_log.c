#include "activity_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "activity_log";

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    return (mkdir(path, 0775) == 0) ? ESP_OK : ESP_FAIL;
}

static void build_csv_name(time_t start_ts, uint32_t id, char *out, size_t out_len)
{
    struct tm tm_local;
    localtime_r(&start_ts, &tm_local);
    snprintf(out, out_len,
             "%04d%02d%02d_%02d%02d_%02u.csv",
             tm_local.tm_year + 1900,
             tm_local.tm_mon + 1,
             tm_local.tm_mday,
             tm_local.tm_hour,
             tm_local.tm_min,
             (unsigned)(id % 100));
}

esp_err_t activity_log_start(activity_log_t *log,
                            sd_mmc_helper_t *sd,
                            time_t start_ts,
                            uint32_t activity_id)
{
    if (!log || !sd || !sd->mounted || !sd->mount_point) return ESP_ERR_INVALID_STATE;

    memset(log, 0, sizeof(*log));
    log->flush_every_n = 10; // default: flush every 10 strokes

    // Ensure /activities directory exists
    char dir_full[128];
    snprintf(dir_full, sizeof(dir_full), "%s/activities", sd->mount_point);
    esp_err_t e = ensure_dir(dir_full);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ensure_dir failed: %s", dir_full);
        return e;
    }

    char name[32];
    build_csv_name(start_ts, activity_id, name, sizeof(name));
    snprintf(log->rel_path, sizeof(log->rel_path), "activities/%s", name);

    // Open file
    char full_path[160];
    snprintf(full_path, sizeof(full_path), "%s/%s", sd->mount_point, log->rel_path);

    log->fp = fopen(full_path, "w");
    if (!log->fp) {
        ESP_LOGE(TAG, "fopen failed: %s", full_path);
        return ESP_FAIL;
    }

    // Line buffered is usually nice for logs
    setvbuf(log->fp, NULL, _IOLBF, 0);

    // Header
    // epoch_ts,session_time_s,stroke_idx,spm_raw,stroke_period_s,speed_mps,distance_m
    fprintf(log->fp,
            "epoch_ts,session_time_s,stroke_idx,spm_raw,stroke_period_s,speed_mps,distance_m,drive_time_s,recovery_time_s\n");

    log->opened = true;
    log->pending = 0;

    ESP_LOGI(TAG, "Per-stroke log: %s", full_path);
    return ESP_OK;
}

esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row)
{
    if (!log || !row) return ESP_ERR_INVALID_ARG;
    if (!log->opened || !log->fp) return ESP_ERR_INVALID_STATE;

    // Keep full decimals for spm_raw in file:
    // Use %.3f for SPM raw, adjust later if you want more precision.
    fprintf(log->fp,
            "%ld,%.3f,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            (long)row->epoch_ts,
            (double)row->session_time_s,
            (unsigned long)row->session_stroke_idx,
            (double)row->spm_raw,
            (double)row->stroke_period_s,
            (double)row->speed_mps,
            (double)row->distance_m,
            (double)row->drive_time_s,
            (double)row->recovery_time_s
        );

    log->pending++;
    if (log->flush_every_n > 0 && log->pending >= log->flush_every_n) {
        fflush(log->fp);
        log->pending = 0;
    }
    return ESP_OK;
}

esp_err_t activity_log_stop(activity_log_t *log)
{
    if (!log) return ESP_ERR_INVALID_ARG;
    if (log->fp) {
        fflush(log->fp);
        fclose(log->fp);
    }
    log->fp = NULL;
    log->opened = false;
    log->pending = 0;
    return ESP_OK;
}
