// components/activity_log/include/activity_log.h
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "sd_mmc_helper.h" 
#include "activity.h"
#include "esp_err.h"

// Struct for Split Data (The "Summary Row")
typedef struct {
    int split_index;          
    float total_dist_m;       
    float split_dist_m;       
    float split_time_s;       
    float split_pace_s;       
    float avg_spm;
    uint16_t avg_hr;          /* 0 when the strap had no samples in this split */
} activity_log_split_row_t;

// Existing Stroke Row
typedef struct {
    time_t rtc_time;
    float session_time_s;
    float total_distance_m;
    float pace_500m_s;
    float spm_instant;
    float avg_pace_500m_s;
    float avg_speed_mps;
    float stroke_length_m;
    uint32_t stroke_count;
    double gps_lat;
    double gps_lon;
    float power_w;
    float drive_time_s;
    float recovery_time_s;
    float recovery_ratio;
    uint16_t hr_bpm;          /* 0 when no fresh sample */
    bool pod_valid;
    float pod_catch_deg;
    float pod_finish_deg;
    float pod_arc_deg;
} activity_log_row_t;

typedef struct {
    uint16_t round_index;
    char phase[6];            // "WORK" or "REST"
    uint32_t target_value;
    char target_unit[8];      // "s", "m", "st"
    float phase_time_s;
    float phase_distance_m;
    float phase_pace_s;
    float avg_spm;
    uint16_t avg_hr;          /* 0 when the strap had no samples in this phase */
} activity_log_interval_row_t;

typedef struct {
    uint32_t work_value;
    char work_unit[8];
    uint32_t rest_value;
    char rest_unit[8];
    uint16_t rounds;
    bool auto_advance;
} activity_log_interval_cfg_t;

typedef enum {
    ACT_LOG_ROW_STROKE = 0,
    ACT_LOG_ROW_INTERVAL,
} activity_log_row_kind_t;

typedef struct {
    activity_log_row_kind_t kind;
    union {
        activity_log_row_t stroke;
        activity_log_interval_row_t interval;
    } data;
} activity_log_msg_t;

// Main Log Handle
typedef struct {
    bool opened;
    FILE *f_main;             // <--- Updated
    FILE *f_splits;           // <--- Updated
    char filename_base[128];   
    uint32_t flush_every_n;   
    uint32_t pending;         
    char rel_path[96];        // kept for backward compat if needed

    bool has_last_row;
    float last_row_distance_m;
    float last_row_time_s;
    uint32_t last_row_stroke_count;

    bool interval_cfg_valid;
    activity_log_interval_cfg_t interval_cfg;

    activity_type_t activity_type;
    float split_interval_m;      // Configured interval (e.g. 1000m)
    float last_split_dist_m;     // Distance when last split occurred
    float last_split_time_s;     // Time when last split occurred
    int   next_split_index;      // 1, 2, 3...

    uint32_t hr_sum;             /* non-zero samples since the last split row */
    uint16_t hr_n;
} activity_log_t;

void activity_log_init(activity_log_t *log);
esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_time, uint32_t session_id, activity_type_t act_type);
esp_err_t activity_log_stop(activity_log_t *log);
esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row);
esp_err_t activity_log_append_split(activity_log_t *log, const activity_log_split_row_t *row);
esp_err_t activity_log_finalize(activity_log_t *log, float final_distance_m, float final_time_s, float avg_spm);
void activity_log_set_interval_config(activity_log_t *log, const activity_log_interval_cfg_t *cfg);
esp_err_t activity_log_append_interval(activity_log_t *log, const activity_log_interval_row_t *row);

/* Configure automatic splits (e.g., every 500m). 0 to disable. */
void activity_log_set_split_interval(activity_log_t *log, uint32_t interval_m);
