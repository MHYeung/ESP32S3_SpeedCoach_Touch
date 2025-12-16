// components/stroke_detection/stroke_detection.c
#include "stroke_detection.h"
#include <math.h>
#include <string.h>

typedef enum { PHASE_RECOVERY = 0, PHASE_DRIVE } phase_t;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float lpf_alpha(float dt, float tau_s) { return dt / (tau_s + dt); }

static float one_pole_lpf(float x, float *y, float alpha) {
    *y += alpha * (x - *y);
    return *y;
}

// 1st-order HPF via LPF subtraction: y = x - lpf(x)
static float one_pole_hpf(float x, float *lpf_state, float alpha_lpf) {
    float lp = one_pole_lpf(x, lpf_state, alpha_lpf);
    return x - lp;
}

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

    // add new
    sd->buf_x[i] = ax; sd->buf_y[i] = ay; sd->buf_z[i] = az;
    sd->sum[0]   += ax; sd->sumsq[0] += ax*ax;
    sd->sum[1]   += ay; sd->sumsq[1] += ay*ay;
    sd->sum[2]   += az; sd->sumsq[2] += az*az;

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

void stroke_detection_init(stroke_detection_t *sd_, const stroke_detection_cfg_t *cfg)
{
    stroke_detection_t *sd = sd_;
    memset(sd, 0, sizeof(*sd));
    sd->cfg = *cfg;

    sd->win_n = (int)lroundf(cfg->fs_hz * cfg->axis_window_s);
    sd->win_n = (int)clampf((float)sd->win_n, 32.0f, 1024.0f);
    sd->hold_n = (int)lroundf(cfg->fs_hz * cfg->axis_hold_s);
    if (sd->hold_n < 1) sd->hold_n = 1;

    sd->best_axis = 0;
    sd->phase = (int)PHASE_RECOVERY;
    sd->t_last_catch = -1.0f;
    sd->t_prev_catch = -1.0f;
    sd->t_last_finish = -1.0f;

    sd->last.spm = NAN;
    sd->last.stroke_period_s = NAN;
    sd->last.drive_time_s = NAN;
    sd->last.recovery_time_s = NAN;
    sd->last.a_long = NAN;
    sd->last.a_long_f = NAN;
    sd->last.g_mag = NAN;

    // initialize buffers to 0 (already from memset)
}

stroke_event_t stroke_detection_update(stroke_detection_t *sd_,
                                       float t_s,
                                       float ax, float ay, float az,
                                       float gx, float gy, float gz,
                                       stroke_metrics_t *out)
{
    stroke_detection_t *sd = sd_;
    if (out) *out = sd->last;

    float dt = 1.0f / sd->cfg.fs_hz;
    if (sd->has_prev_t) {
        float dt_meas = t_s - sd->prev_t;
        if (dt_meas > 0.0005f && dt_meas < 0.1f) dt = dt_meas;
    }
    sd->has_prev_t = true;
    sd->prev_t = t_s;

    // Stage 1: gravity estimate + dynamic accel
    float a_raw[3] = {ax, ay, az};
    float alpha_g = lpf_alpha(dt, sd->cfg.gravity_tau_s);
    for (int i=0;i<3;i++) sd->g_est[i] += alpha_g * (a_raw[i] - sd->g_est[i]);
    float a_dyn[3] = { ax - sd->g_est[0], ay - sd->g_est[1], az - sd->g_est[2] };

    // Stage 2: axis auto-detect via variance window
    axis_window_push(sd, a_dyn[0], a_dyn[1], a_dyn[2]);
    float vx = axis_variance(sd, 0), vy = axis_variance(sd, 1), vz = axis_variance(sd, 2);
    int candidate = (vy > vx) ? 1 : 0;
    if ((candidate == 0 ? vx : vy) < vz) candidate = 2;

    if (candidate == sd->best_axis) {
        sd->hold_count = 0;
    } else {
        if (++sd->hold_count >= sd->hold_n) {
            sd->best_axis = candidate;
            sd->hold_count = 0;
        }
    }

    // Stage 3: virtual projection (max-variance axis version)
    float a_long = a_dyn[sd->best_axis];

    // Stage 4: band-limit (HPF then LPF)
    float alpha_hpf = lpf_alpha(dt, 1.0f / (2.0f * (float)M_PI * sd->cfg.hpf_hz)); // tau from fc
    float a_hp = one_pole_hpf(a_long, &sd->hpf_lp_state, alpha_hpf);

    float alpha_lpf2 = lpf_alpha(dt, 1.0f / (2.0f * (float)M_PI * sd->cfg.lpf_hz));
    float a_f = one_pole_lpf(a_hp, &sd->lpf_y, alpha_lpf2);

    // gyro magnitude (optional confirmation)
    float g_mag = sqrtf(gx*gx + gy*gy + gz*gz);

    // adaptive threshold from EWMA of a_f^2
    float beta = lpf_alpha(dt, 0.8f); // ~0.8s smoothing
    sd->rms2_ewma += beta * ((a_f * a_f) - sd->rms2_ewma);
    float thr = sd->cfg.thr_k * sqrtf(fmaxf(sd->rms2_ewma, 0.0f));
    if (thr < sd->cfg.thr_floor) thr = sd->cfg.thr_floor;

    stroke_event_t ev = STROKE_EVENT_NONE;
    const float prev_a_f = sd->prev_a_f;
    sd->prev_a_f = a_f;

    // simple FSM
    if ((phase_t)sd->phase == PHASE_RECOVERY) {
        // Catch: rising edge through +thr
        if (prev_a_f <= thr && a_f > thr) {
            bool accept = true;
            if (sd->have_prev_catch) {
                float period = t_s - sd->t_prev_catch;
                accept = (period >= sd->cfg.min_stroke_period_s && period <= sd->cfg.max_stroke_period_s);
            }

            if (accept) {
                if (sd->have_prev_catch) {
                    float period = t_s - sd->t_prev_catch;
                    sd->last.stroke_period_s = period;
                    sd->last.spm = 60.0f / period;
                }

                if (sd->have_finish) {
                    sd->last.recovery_time_s = t_s - sd->t_last_finish;
                }

                sd->t_prev_catch = t_s;
                sd->have_prev_catch = true;

                sd->phase = (int)PHASE_DRIVE;
                sd->t_last_catch = t_s;
                sd->have_catch = true;
                ev = STROKE_EVENT_CATCH;
            }
        }
    } else { // PHASE_DRIVE
        // Finish: falling edge through -thr
        if (prev_a_f >= -thr && a_f < -thr) {
            if (sd->have_catch) {
                sd->last.drive_time_s = t_s - sd->t_last_catch;
            }

            sd->phase = (int)PHASE_RECOVERY;
            sd->t_last_finish = t_s;
            sd->have_finish = true;
            ev = STROKE_EVENT_FINISH;
        }
    }

    // telemetry
    sd->last.a_long = a_long;
    sd->last.a_long_f = a_f;
    sd->last.g_mag = g_mag;

    if (out) {
        *out = sd->last;
    }

    return ev;
}
