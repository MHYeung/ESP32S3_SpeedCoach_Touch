#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "sd_mmc_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // time
    time_t   epoch_ts;          // RTC/system epoch at stroke time
    float    session_time_s;    // time since activity start (your session timer)

    // stroke
    uint32_t session_stroke_idx;  // 1..N within session
    float    spm_raw;             // full precision (for saving)
    float    stroke_period_s;     // period between strokes (if available)

    // placeholders for future (GPS etc.)
    float    speed_mps;
    float    distance_m;          // cumulative session distance (m)

    //Drive to Recovery Ratio Calculation
    float drive_time_s;
    float recovery_time_s;
} activity_log_row_t;

typedef struct {
    bool  opened;
    FILE *fp;
    char  rel_path[96];     // e.g. "activities/202512251051_01.csv"
    uint32_t flush_every_n; // flush after N rows
    uint32_t pending;
} activity_log_t;

/**
 * Open per-stroke CSV file and write header.
 * File name uses start time: YYYYMMDDHHMM_xx.csv  (xx = id % 100)
 */
esp_err_t activity_log_start(activity_log_t *log,
                            sd_mmc_helper_t *sd,
                            time_t start_ts,
                            uint32_t activity_id);

/** Append one per-stroke row (call from logger task only). */
esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row);

/** Close file (flush). Safe to call even if not opened. */
esp_err_t activity_log_stop(activity_log_t *log);

#ifdef __cplusplus
}
#endif
