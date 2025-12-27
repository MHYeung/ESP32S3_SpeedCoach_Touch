// components/activity_log/include/activity_log.h
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "sd_mmc_helper.h" 
#include "esp_err.h"

// Struct for Split Data (The "Summary Row")
typedef struct {
    int split_index;          
    float total_dist_m;       
    float split_dist_m;       
    float split_time_s;       
    float split_pace_s;       
    float avg_spm;            
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
} activity_log_row_t;

// Main Log Handle
typedef struct {
    bool opened;
    FILE *f_main;             // <--- Updated
    FILE *f_splits;           // <--- Updated
    char filename_base[128];   
    uint32_t flush_every_n;   
    uint32_t pending;         
    char rel_path[96];        // kept for backward compat if needed

    float split_interval_m;      // Configured interval (e.g. 1000m)
    float last_split_dist_m;     // Distance when last split occurred
    float last_split_time_s;     // Time when last split occurred
    int   next_split_index;      // 1, 2, 3...
} activity_log_t;

void activity_log_init(activity_log_t *log);
esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_time, uint32_t session_id);
esp_err_t activity_log_stop(activity_log_t *log);
esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row);
esp_err_t activity_log_append_split(activity_log_t *log, const activity_log_split_row_t *row);

/* Configure automatic splits (e.g., every 500m). 0 to disable. */
void activity_log_set_split_interval(activity_log_t *log, uint32_t interval_m);