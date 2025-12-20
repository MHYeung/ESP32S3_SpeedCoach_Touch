// components/stroke_detection/include/stroke_detection.h
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef STROKE_THR_K_DEFAULT
#define STROKE_THR_K_DEFAULT 1.0f
#endif

#ifndef STROKE_THR_FLOOR_DEFAULT
#define STROKE_THR_FLOOR_DEFAULT 0.85f
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

    // debug/telemetry (optional but useful)
    float a_long;        // projected dynamic accel (m/s^2)
    float a_long_f;      // filtered version
    float g_mag;         // gyro magnitude (rad/s)
} stroke_metrics_t;

typedef struct {
    float fs_hz;

    // gravity/tilt rejection
    float gravity_tau_s;         // ~0.5–1.0

    // variance window for axis auto-detect
    float axis_window_s;         // 3–5 seconds
    float axis_hold_s;           // e.g. 0.5 seconds stable before switching
    bool accel_use_fixed_axis;   // if true, skip auto-detect and use accel_fixed_axis
    int accel_fixed_axis;        // 0=x, 1=y, 2=z

    // filters for a_long (simple HPF + LPF)
    float hpf_hz;                // ~0.5
    float lpf_hz;                // ~8–10

    // event detection constraints
    float min_stroke_period_s;   // e.g. 0.4 (150 spm cap)
    float max_stroke_period_s;   // e.g. 6.0 (10 spm floor)

    // adaptive thresholding
    float thr_k;                 // e.g. 1.2 (multiplier on RMS)
    float thr_floor;             // e.g. 0.5 (m/s^2)
} stroke_detection_cfg_t;

/*
 * Stroke detector state.
 * Kept as a concrete struct so it can be allocated statically (no malloc).
 * Note: The axis-variance window uses fixed 1024-sample buffers (per axis).
 */
typedef struct stroke_detection {
    stroke_detection_cfg_t cfg;

    bool has_prev_t;
    float prev_t;

    float g_est[3];
    int polarity; // +1 or -1, learns stroke direction

    int win_n;
    int win_i;
    int win_count;
    int hold_n;
    int hold_count;
    int best_axis; // 0=x 1=y 2=z

    float buf_x[1024];
    float buf_y[1024];
    float buf_z[1024];
    float sum[3];
    float sumsq[3];

    float hpf_lp_state;
    float lpf_y;
    float prev_a_f;

    float rms2_ewma;

    float g_rms2_ewma[3];
    int best_g_axis; // 0=x 1=y 2=z
    int g_hold_count;
    float g_hpf_lp_state;
    float g_lpf_y;
    float prev_g_f;
    float prev2_g_f;
    bool polarity_locked;
    bool accel_axis_fixed;

    uint32_t stroke_count;
    float t_last_stroke;
    bool have_last_stroke;

    int phase; // internal

    float t_last_catch;
    float t_prev_catch;
    float t_last_finish;
    bool have_catch;
    bool have_prev_catch;
    bool have_finish;

    float peak_norm; // peak of (polarity * a_f) during drive
    float t_last_event; // last accepted catch/finish (s)

    float period_hist[3];
    int period_hist_count;
    int period_hist_i;
    float period_hist_sum;

    stroke_metrics_t last;
} stroke_detection_t;

void stroke_detection_init(stroke_detection_t *sd, const stroke_detection_cfg_t *cfg);
stroke_event_t stroke_detection_update(stroke_detection_t *sd,
                                       float t_s,
                                       float ax, float ay, float az,     // m/s^2
                                       float gx, float gy, float gz,     // rad/s
                                       stroke_metrics_t *out);

#ifdef __cplusplus
}
#endif
