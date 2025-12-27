// components/stroke_detection/include/stroke_detection.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- MODIFIED DEFAULTS FOR HULL MOUNTING ---
#ifndef STROKE_THR_K_DEFAULT
#define STROKE_THR_K_DEFAULT 1.3f 
#endif

// Floor: 0.35 m/s^2 (approx 0.035g).
#ifndef STROKE_THR_FLOOR_DEFAULT
#define STROKE_THR_FLOOR_DEFAULT 0.35f 
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STROKE_EVENT_NONE = 0,
    STROKE_EVENT_CATCH,
    STROKE_EVENT_FINISH,
} stroke_event_t;

typedef struct {
    float spm;
    float stroke_period_s;
    float drive_time_s;
    float recovery_time_s;
    uint32_t stroke_count;

    // Telemetry
    float a_long;        // Raw longitudinal acceleration
    float a_long_f;      // Filtered surge signal (The Trigger)
    float g_mag;         // Gyro magnitude
} stroke_metrics_t;

typedef struct {
    float fs_hz;

    // Gravity Rejection
    float gravity_tau_s;         // ~1.0s

    // Axis Auto-Detect
    float axis_window_s;         // Window to decide X vs Y vs Z
    
    // --- ADDED MISSING FIELD HERE ---
    float axis_hold_s;           // Time to hold new axis before switching (e.g., 1.0s)
    // --------------------------------

    bool accel_use_fixed_axis;   // Set TRUE if you know orientation
    int accel_fixed_axis;        // 0=x, 1=y, 2=z

    // Filters for Surge
    float hpf_hz;                // ~0.1 Hz
    float lpf_hz;                // ~3.0 Hz

    float min_stroke_period_s;   // e.g. 1.0s
    float max_stroke_period_s;   // e.g. 6.0s

    float thr_k;
    float thr_floor;
} stroke_detection_cfg_t;

typedef struct stroke_detection {
    stroke_detection_cfg_t cfg;

    bool has_prev_t;
    float prev_t;

    // Gravity Estimation
    float g_est[3];
    int polarity; 

    // Axis Selection
    int win_n;
    int win_i;
    int win_count;
    int hold_n;
    int hold_count;
    int best_axis; 

    float buf_x[1024];
    float buf_y[1024];
    float buf_z[1024];
    float sum[3];
    float sumsq[3];

    // Acceleration Filters
    float hpf_lp_state;
    float lpf_y;
    float prev_a_f;
    float prev2_a_f;

    // Adaptive Threshold
    float rms2_ewma;
    bool polarity_locked;

    // State Machine
    int phase; 
    uint32_t stroke_count;
    
    // Timing
    float t_last_catch;
    float t_last_finish;
    float t_last_event;

    // Peak Tracking
    float peak_norm; 

    // SPM Smoothing
    float period_hist[3];
    int period_hist_count;
    int period_hist_i;
    float period_hist_sum;

    stroke_metrics_t last;
} stroke_detection_t;

void stroke_detection_init(stroke_detection_t *sd, const stroke_detection_cfg_t *cfg);
stroke_event_t stroke_detection_update(stroke_detection_t *sd,
                                       float t_s,
                                       float ax, float ay, float az,
                                       float gx, float gy, float gz,
                                       stroke_metrics_t *out);

#ifdef __cplusplus
}
#endif