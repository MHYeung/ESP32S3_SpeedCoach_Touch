#include "activity.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_log.h"

static const char *TAG = "activity";

static void clamp_nonneg_f(float *v) {
    if (!v) return;
    if (*v < 0.0f) *v = 0.0f;
}

static void update_max(float *max_v, float v) {
    if (!max_v) return;
    if (v > *max_v) *max_v = v;
}

static void recompute_avgs(activity_t *a)
{
    if (!a) return;
    if (a->total_dt <= 1e-6) {
        a->avg_speed_mps = 0.0f;
        a->avg_spm = 0.0f;
        a->avg_power_w = 0.0f;
        return;
    }
    a->avg_speed_mps = (float)(a->sum_speed_dt / a->total_dt);
    a->avg_spm       = (float)(a->sum_spm_dt   / a->total_dt);
    a->avg_power_w   = (float)(a->sum_power_dt / a->total_dt);
}

static void fmt_iso8601_utc(time_t ts, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (ts <= 0) {
        snprintf(out, out_len, "1970-01-01T00:00:00Z");
        return;
    }
    struct tm tm_utc;
    gmtime_r(&ts, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

void activity_init(activity_t *a, uint32_t id)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->id = id;
    a->state = ACTIVITY_STATE_IDLE;
}

esp_err_t activity_start(activity_t *a, time_t start_ts)
{
    if (!a) return ESP_ERR_INVALID_ARG;

    // Reset everything except ID
    uint32_t id = a->id;
    memset(a, 0, sizeof(*a));
    a->id = id;

    if (start_ts == 0) start_ts = time(NULL);

    a->start_ts = start_ts;
    a->state = ACTIVITY_STATE_RECORDING;

    // Max fields should start at 0
    a->max_speed_mps = 0.0f;
    a->max_spm = 0.0f;
    a->max_power_w = 0.0f;

    ESP_LOGI(TAG, "activity_start id=%lu start_ts=%ld",
             (unsigned long)a->id, (long)a->start_ts);

    return ESP_OK;
}

esp_err_t activity_update(activity_t *a,
                          float dt_s,
                          float speed_mps,
                          float spm,
                          float power_w,
                          float distance_delta_m,
                          uint32_t stroke_delta)
{
    if (!a) return ESP_ERR_INVALID_ARG;
    if (a->state != ACTIVITY_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    if (dt_s <= 0.0f) {
        // allow dt=0 updates (just max/last), but don’t accumulate avgs
        dt_s = 0.0f;
    }

    // sanitize
    if (speed_mps < 0.0f) speed_mps = 0.0f;
    if (spm < 0.0f) spm = 0.0f;
    if (power_w < 0.0f) power_w = 0.0f;
    if (distance_delta_m < 0.0f) distance_delta_m = 0.0f;

    // totals
    a->distance_m += distance_delta_m;
    a->stroke_count += stroke_delta;

    // time
    a->total_dt += (double)dt_s;
    a->duration_ms = (uint32_t)(a->total_dt * 1000.0);

    // weighted avg accumulators
    a->sum_speed_dt += (double)speed_mps * (double)dt_s;
    a->sum_spm_dt   += (double)spm       * (double)dt_s;
    a->sum_power_dt += (double)power_w   * (double)dt_s;

    // max
    update_max(&a->max_speed_mps, speed_mps);
    update_max(&a->max_spm, spm);
    update_max(&a->max_power_w, power_w);

    // refresh averages
    recompute_avgs(a);

    // clamp
    clamp_nonneg_f(&a->distance_m);

    return ESP_OK;
}

esp_err_t activity_stop(activity_t *a, time_t end_ts)
{
    if (!a) return ESP_ERR_INVALID_ARG;
    if (a->state != ACTIVITY_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    if (end_ts == 0) end_ts = time(NULL);
    a->end_ts = end_ts;

    // If duration was not built from updates, derive from timestamps
    if (a->duration_ms == 0 && a->end_ts > a->start_ts) {
        uint32_t ms = (uint32_t)((a->end_ts - a->start_ts) * 1000);
        a->duration_ms = ms;
        a->total_dt = (double)ms / 1000.0;
        // averages already represent what was accumulated; keep them
    }

    recompute_avgs(a);

    a->state = ACTIVITY_STATE_STOPPED;

    ESP_LOGI(TAG, "activity_stop id=%lu end_ts=%ld duration_ms=%lu dist=%.1fm strokes=%lu",
             (unsigned long)a->id, (long)a->end_ts, (unsigned long)a->duration_ms,
             (double)a->distance_m, (unsigned long)a->stroke_count);

    return ESP_OK;
}

esp_err_t activity_to_json(const activity_t *a, char *buf, size_t buf_len)
{
    if (!a || !buf || buf_len == 0) return ESP_ERR_INVALID_ARG;

    char start_iso[32], end_iso[32];
    fmt_iso8601_utc(a->start_ts, start_iso, sizeof(start_iso));
    fmt_iso8601_utc(a->end_ts ? a->end_ts : a->start_ts, end_iso, sizeof(end_iso));

    int n = snprintf(buf, buf_len,
        "{"
          "\"id\":%lu,"
          "\"start\":\"%s\","
          "\"end\":\"%s\","
          "\"duration_ms\":%lu,"
          "\"distance_m\":%.3f,"
          "\"strokes\":%lu,"
          "\"avg_speed_mps\":%.3f,"
          "\"max_speed_mps\":%.3f,"
          "\"avg_spm\":%.2f,"
          "\"max_spm\":%.2f,"
          "\"avg_power_w\":%.2f,"
          "\"max_power_w\":%.2f"
        "}\n",
        (unsigned long)a->id,
        start_iso,
        end_iso,
        (unsigned long)a->duration_ms,
        (double)a->distance_m,
        (unsigned long)a->stroke_count,
        (double)a->avg_speed_mps,
        (double)a->max_speed_mps,
        (double)a->avg_spm,
        (double)a->max_spm,
        (double)a->avg_power_w,
        (double)a->max_power_w
    );

    if (n < 0 || (size_t)n >= buf_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t activity_to_csv_row(const activity_t *a, char *buf, size_t buf_len)
{
    if (!a || !buf || buf_len == 0) return ESP_ERR_INVALID_ARG;

    // CSV columns:
    // id,start_iso,end_iso,duration_ms,distance_m,strokes,avg_speed_mps,max_speed_mps,avg_spm,max_spm,avg_power_w,max_power_w
    char start_iso[32], end_iso[32];
    fmt_iso8601_utc(a->start_ts, start_iso, sizeof(start_iso));
    fmt_iso8601_utc(a->end_ts ? a->end_ts : a->start_ts, end_iso, sizeof(end_iso));

    int n = snprintf(buf, buf_len,
        "%lu,%s,%s,%lu,%.3f,%lu,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f\n",
        (unsigned long)a->id,
        start_iso,
        end_iso,
        (unsigned long)a->duration_ms,
        (double)a->distance_m,
        (unsigned long)a->stroke_count,
        (double)a->avg_speed_mps,
        (double)a->max_speed_mps,
        (double)a->avg_spm,
        (double)a->max_spm,
        (double)a->avg_power_w,
        (double)a->max_power_w
    );

    if (n < 0 || (size_t)n >= buf_len) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t ensure_dir_exists(const char *full_dir)
{
    struct stat st;
    if (stat(full_dir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return ESP_OK;
        return ESP_FAIL;
    }
    // create
    if (mkdir(full_dir, 0775) == 0) return ESP_OK;
    return ESP_FAIL;
}

static void build_csv_name_from_start(time_t start_ts, uint32_t id,
                                      char *out, size_t out_len)
{
    // Use local time for naming (matches your example)
    struct tm tm_local;
    localtime_r(&start_ts, &tm_local);

    // "YYYYMMDDHHMM_xx.csv"
    // xx = id % 100 (keeps it 2 digits)
    snprintf(out, out_len,
             "%04d%02d%02d%02d%02d_%02u.csv",
             tm_local.tm_year + 1900,
             tm_local.tm_mon + 1,
             tm_local.tm_mday,
             tm_local.tm_hour,
             tm_local.tm_min,
             (unsigned)(id % 100));
}


esp_err_t activity_save_to_sd(sd_mmc_helper_t *sd,
                              const activity_t *a,
                              bool append_index_csv)
{
    if (!sd || !sd->mounted || !sd->mount_point) return ESP_ERR_INVALID_STATE;
    if (!a) return ESP_ERR_INVALID_ARG;

    // Create directory: <mount>/activities
    char dir_full[128];
    int dn = snprintf(dir_full, sizeof(dir_full), "%s/activities", sd->mount_point);
    if (dn <= 0 || dn >= (int)sizeof(dir_full)) return ESP_ERR_INVALID_ARG;

    if (ensure_dir_exists(dir_full) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to ensure dir: %s", dir_full);
        return ESP_FAIL;
    }

    char csv_name[32];
    build_csv_name_from_start(a->start_ts, a->id, csv_name, sizeof(csv_name));

    char rel_csv_path[80];
    snprintf(rel_csv_path, sizeof(rel_csv_path), "activities/%s", csv_name);

    char row[384];
    esp_err_t err = activity_to_csv_row(a, row, sizeof(row));
    if (err != ESP_OK) return err;

    // overwrite per-activity CSV (one row file)
    err = sd_mmc_helper_write_text(sd, rel_csv_path, row, false /* overwrite */);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Saved activity id=%lu to SD", (unsigned long)a->id);
    return ESP_OK;
}

