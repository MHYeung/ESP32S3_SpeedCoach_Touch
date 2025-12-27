// components/stroke_detection/stroke_detection.c
#include "stroke_detection.h"
#include <math.h>
#include <string.h>

// --- Helper Macros & Functions ---
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float lpf_alpha(float dt, float tau_s) { return dt / (tau_s + dt); }

// Standard One-Pole Low Pass
static float one_pole_lpf(float x, float *y, float alpha) {
    *y += alpha * (x - *y);
    return *y;
}

// Standard One-Pole High Pass (DC Blocker)
static float one_pole_hpf(float x, float *lpf_state, float alpha_lpf) {
    float lp = one_pole_lpf(x, lpf_state, alpha_lpf);
    return x - lp;
}

// --- Axis Variance Buffer (Finds the Surge Axis) ---
static void axis_window_push(struct stroke_detection *sd, float ax, float ay, float az)
{
    const int i = sd->win_i;
    if (sd->win_count >= sd->win_n) {
        float ox = sd->buf_x[i], oy = sd->buf_y[i], oz = sd->buf_z[i];
        sd->sum[0] -= ox; sd->sumsq[0] -= ox * ox;
        sd->sum[1] -= oy; sd->sumsq[1] -= oy * oy;
        sd->sum[2] -= oz; sd->sumsq[2] -= oz * oz;
    } else {
        sd->win_count++;
    }
    sd->buf_x[i] = ax; sd->buf_y[i] = ay; sd->buf_z[i] = az;
    sd->sum[0] += ax; sd->sumsq[0] += ax*ax;
    sd->sum[1] += ay; sd->sumsq[1] += ay*ay;
    sd->sum[2] += az; sd->sumsq[2] += az*az;
    sd->win_i = (i + 1) % sd->win_n;
}

static float axis_variance(const struct stroke_detection *sd, int axis)
{
    const int n = sd->win_count > 0 ? sd->win_count : 1;
    float mean = sd->sum[axis] / (float)n;
    float ex2  = sd->sumsq[axis] / (float)n;
    float var  = ex2 - mean*mean;
    return var > 0 ? var : 0;
}

// --- SPM Averaging ---
static void period_hist_reset(stroke_detection_t *sd) {
    sd->period_hist_count = 0;
    sd->period_hist_i = 0;
    sd->period_hist_sum = 0.0f;
}

static void period_hist_push(stroke_detection_t *sd, float period) {
    if (sd->period_hist_count < 3) {
        sd->period_hist[sd->period_hist_i] = period;
        sd->period_hist_sum += period;
        sd->period_hist_count++;
    } else {
        sd->period_hist_sum -= sd->period_hist[sd->period_hist_i];
        sd->period_hist[sd->period_hist_i] = period;
        sd->period_hist_sum += period;
    }
    sd->period_hist_i = (sd->period_hist_i + 1) % 3;
}

static float period_hist_mean(const stroke_detection_t *sd) {
    if (sd->period_hist_count <= 0) return NAN;
    return sd->period_hist_sum / (float)sd->period_hist_count;
}

// --- Initialization ---
void stroke_detection_init(stroke_detection_t *sd_, const stroke_detection_cfg_t *cfg)
{
    stroke_detection_t *sd = sd_;
    memset(sd, 0, sizeof(*sd));
    sd->cfg = *cfg;

    // Safety Defaults
    if (sd->cfg.gravity_tau_s <= 0) sd->cfg.gravity_tau_s = 1.0f; 
    if (sd->cfg.fs_hz <= 0) sd->cfg.fs_hz = 100.0f;
    if (sd->cfg.thr_floor <= 0.01f) sd->cfg.thr_floor = 0.35f;

    // Buffer sizing
    sd->win_n = (int)lroundf(cfg->fs_hz * cfg->axis_window_s);
    sd->win_n = (int)clampf((float)sd->win_n, 32.0f, 1024.0f);

    // --- CORRECTION HERE ---
    // Use the config value (axis_hold_s) instead of hardcoded 1.0f
    float hold_s = (cfg->axis_hold_s > 0.0f) ? cfg->axis_hold_s : 1.0f; 
    sd->hold_n = (int)lroundf(cfg->fs_hz * hold_s);
    if (sd->hold_n < 1) sd->hold_n = 1;
    // -----------------------

    // Default to Z axis (often vertical) or user fixed
    sd->best_axis = (cfg->accel_use_fixed_axis) ? cfg->accel_fixed_axis : 0; 
    
    sd->polarity = +1;
    sd->phase = 0; // 0 = Recovery, 1 = Drive
    
    sd->t_last_catch = -1.0f;
    sd->t_last_finish = -1.0f;
    sd->t_last_event = -1.0f;

    period_hist_reset(sd);
}

// --- Update Logic (HULL MODE) ---
stroke_event_t stroke_detection_update(stroke_detection_t *sd_,
                                       float t_s,
                                       float ax, float ay, float az,
                                       float gx, float gy, float gz,
                                       stroke_metrics_t *out)
{
    stroke_detection_t *sd = sd_;
    stroke_event_t ev = STROKE_EVENT_NONE;

    // 1. Time Delta
    float dt = 1.0f / sd->cfg.fs_hz;
    if (sd->has_prev_t) {
        float dt_meas = t_s - sd->prev_t;
        if (dt_meas > 0.0005f && dt_meas < 0.1f) dt = dt_meas;
    }
    sd->has_prev_t = true;
    sd->prev_t = t_s;

    // 2. Remove Gravity (Estimate Gravity Vector)
    float a_raw[3] = {ax, ay, az};
    float alpha_g = lpf_alpha(dt, sd->cfg.gravity_tau_s);
    for (int i=0; i<3; i++) sd->g_est[i] += alpha_g * (a_raw[i] - sd->g_est[i]);

    // Dynamic Acceleration (Linear Movement)
    float a_dyn[3] = { ax - sd->g_est[0], ay - sd->g_est[1], az - sd->g_est[2] };
    
    // 3. Select Surge Axis
    // If not fixed, pick axis with highest energy (variance)
    axis_window_push(sd, a_dyn[0], a_dyn[1], a_dyn[2]);
    if (!sd->cfg.accel_use_fixed_axis) {
        float vx = axis_variance(sd, 0);
        float vy = axis_variance(sd, 1);
        float vz = axis_variance(sd, 2);
        
        // Simple logic: Max variance is usually surge (if Z is mostly gravity)
        int candidate = (vy > vx) ? 1 : 0;
        if ((candidate == 0 ? vx : vy) < vz) candidate = 2;

        if (candidate == sd->best_axis) sd->hold_count = 0;
        else if (++sd->hold_count >= sd->hold_n) {
            sd->best_axis = candidate;
            sd->hold_count = 0;
        }
    }
    
    // Raw Surge Signal
    float a_long = a_dyn[sd->best_axis]; 

    // 4. Bandpass Filter the Surge (Crucial for Hull)
    // HPF: Remove lingering DC/Drag bias
    // LPF: Remove engine vibration/water chop
    float alpha_hpf = lpf_alpha(dt, 1.0f / (2.0f * (float)M_PI * sd->cfg.hpf_hz)); 
    float alpha_lpf = lpf_alpha(dt, 1.0f / (2.0f * (float)M_PI * sd->cfg.lpf_hz));

    float a_hp = one_pole_hpf(a_long, &sd->hpf_lp_state, alpha_hpf);
    float a_f = one_pole_lpf(a_hp, &sd->lpf_y, alpha_lpf);

    // 5. Adaptive Noise Floor
    float beta = lpf_alpha(dt, 1.5f); // Slow adaptation
    sd->rms2_ewma += beta * ((a_f * a_f) - sd->rms2_ewma);
    float thr = sd->cfg.thr_k * sqrtf(fmaxf(sd->rms2_ewma, 0.001f));
    if (thr < sd->cfg.thr_floor) thr = sd->cfg.thr_floor;

    // 6. Polarity Detection (Forward vs Backward)
    // The "Drive" is a large acceleration impulse. The "Recovery" is low drag.
    // If we see a massive negative spike, the sensor is likely backward.
    if (!sd->polarity_locked && (fabsf(a_f) > sd->cfg.thr_floor * 2.0f)) {
        // If the peak is significantly negative, flip polarity
        if (a_f < -1.0f * sd->cfg.thr_floor) {
             sd->polarity = -1;
             sd->polarity_locked = true;
        } else if (a_f > sd->cfg.thr_floor) {
             sd->polarity = 1;
             sd->polarity_locked = true;
        }
    }

    // s0 is our "Rectified Surge" (Positive = Drive)
    float s0 = (float)sd->polarity * a_f;

    // 7. State Machine
    
    // Reset if idle too long
    if (sd->t_last_event > 0.0f && (t_s - sd->t_last_event) > 6.0f) {
        sd->phase = 0;
        sd->peak_norm = 0.0f;
    }

    if (sd->phase == 0) { // RECOVERY STATE -> Looking for Catch
        
        // Trigger: Signal rises above threshold AND slope is positive
        if (s0 > thr && (s0 - sd->prev_a_f * sd->polarity) > 0) {
            
            // --- CATCH DETECTED ---
            sd->phase = 1; 
            float t_now = t_s;

            // Rec Time
            if (sd->t_last_finish > 0.0f) {
                float rec_t = t_now - sd->t_last_finish;
                if (rec_t > 0.1f) sd->last.recovery_time_s = rec_t;
            }

            // Period / SPM
            if (sd->t_last_catch > 0.0f) {
                float period = t_now - sd->t_last_catch;
                if (period >= sd->cfg.min_stroke_period_s && period <= sd->cfg.max_stroke_period_s) {
                    period_hist_push(sd, period);
                    float mean_period = period_hist_mean(sd);
                    sd->last.stroke_period_s = mean_period;
                    sd->last.spm = (mean_period > 0) ? (60.0f / mean_period) : 0.0f;
                    sd->stroke_count++;
                    ev = STROKE_EVENT_CATCH;
                }
            } else {
                // First stroke initialization
                sd->stroke_count++;
                ev = STROKE_EVENT_CATCH; 
            }

            sd->t_last_catch = t_now;
            sd->t_last_event = t_now;
            sd->peak_norm = 0.0f;
        }

    } else { // DRIVE STATE -> Looking for Finish
        
        // Track peak accel during drive
        if (s0 > sd->peak_norm) sd->peak_norm = s0;

        // Trigger: Signal drops below % of peak OR below absolute floor
        // Hull finish is less sharp than oar finish, so we use a lower % or 0-crossing
        float finish_thr = fmaxf(sd->peak_norm * 0.25f, sd->cfg.thr_floor * 0.5f);

        if (s0 < finish_thr) {
            
            // --- FINISH DETECTED ---
            sd->phase = 0;
            float t_now = t_s;

            if (sd->t_last_catch > 0.0f) {
                float drv_t = t_now - sd->t_last_catch;
                if (drv_t > 0.1f) sd->last.drive_time_s = drv_t;
            }
            
            sd->t_last_finish = t_now;
            sd->t_last_event = t_now;
            ev = STROKE_EVENT_FINISH;
        }
    }

    // Save History
    sd->prev2_a_f = sd->prev_a_f;
    sd->prev_a_f = a_f;

    // Telemetry Output
    sd->last.a_long = a_long;
    sd->last.a_long_f = a_f; // View this in Data Page "Power" slot to debug!
    sd->last.g_mag = sqrtf(gx*gx + gy*gy + gz*gz); // just for reference
    sd->last.stroke_count = sd->stroke_count;

    if (out) *out = sd->last;
    return ev;
}