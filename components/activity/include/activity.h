#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "sd_mmc_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTIVITY_STATE_IDLE = 0,
    ACTIVITY_STATE_RECORDING,
    ACTIVITY_STATE_STOPPED,
} activity_state_t;

/**
 * Rowing activity/session summary + running stats.
 * Think of this as your "Activity class" in C.
 */
typedef struct {
    // Identity
    uint32_t id;

    // Time
    time_t   start_ts;        // epoch seconds
    time_t   end_ts;          // epoch seconds
    uint32_t duration_ms;     // derived from updates or end-start

    // Totals
    float    distance_m;
    uint32_t stroke_count;

    // Stats
    float    avg_speed_mps;
    float    max_speed_mps;

    float    avg_spm;
    float    max_spm;

    float    avg_power_w;
    float    max_power_w;

    // Internal accumulators (don’t edit directly)
    activity_state_t state;
    double   sum_speed_dt;    // speed * dt
    double   sum_spm_dt;      // spm   * dt
    double   sum_power_dt;    // power * dt
    double   total_dt;        // seconds
} activity_t;

/**
 * Initialize an activity object (ID must be set by caller or generated).
 */
void activity_init(activity_t *a, uint32_t id);

/**
 * Start a session. If start_ts==0, uses current time(NULL) (requires time set).
 * Resets totals/stats and switches to RECORDING state.
 */
esp_err_t activity_start(activity_t *a, time_t start_ts);

/**
 * Update activity with a time delta (seconds) and instantaneous metrics.
 * Call this every 1–2 strokes (or on a timer).
 *
 * - dt_s: elapsed time since last update
 * - speed_mps/spm/power_w: latest measured values
 * - distance_delta_m: distance increment since last update (can be 0 if unknown)
 * - stroke_delta: number of strokes since last update (0/1/2)
 */
esp_err_t activity_update(activity_t *a,
                          float dt_s,
                          float speed_mps,
                          float spm,
                          float power_w,
                          float distance_delta_m,
                          uint32_t stroke_delta);

/**
 * Stop a session. If end_ts==0, uses time(NULL).
 * Computes avg stats if not computed yet and switches to STOPPED state.
 */
esp_err_t activity_stop(activity_t *a, time_t end_ts);

/**
 * Convenience getters.
 */
static inline bool activity_is_recording(const activity_t *a) {
    return a && a->state == ACTIVITY_STATE_RECORDING;
}

/**
 * Serialize summary to JSON (single object).
 * buf must be provided by caller.
 */
esp_err_t activity_to_json(const activity_t *a, char *buf, size_t buf_len);

/**
 * Serialize one-line CSV record (no header).
 */
esp_err_t activity_to_csv_row(const activity_t *a, char *buf, size_t buf_len);

/**
 * Save files to SD card.
 *
 * - Saves JSON summary to:
 *   <mount>/activities/activity_<id>.json
 *
 * - Optionally appends CSV row to:
 *   <mount>/activities/index.csv
 *
 * (Creates "/activities" directory if missing.)
 */
esp_err_t activity_save_to_sd(sd_mmc_helper_t *sd,
                              const activity_t *a,
                              bool append_index_csv);

#ifdef __cplusplus
}
#endif
