#include "activity.h"
#include <string.h>

void activity_init(activity_t *a, uint32_t id) {
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->id = id;
    a->state = ACTIVITY_STATE_IDLE;
}

esp_err_t activity_start(activity_t *a, time_t start_ts) {
    if (!a) return ESP_ERR_INVALID_ARG;
    uint32_t id = a->id;
    memset(a, 0, sizeof(*a));
    a->id = id;
    a->start_ts = (start_ts == 0) ? time(NULL) : start_ts;
    a->state = ACTIVITY_STATE_RECORDING;
    return ESP_OK;
}

esp_err_t activity_update(activity_t *a, float dt_s, float speed_mps, float spm, float power_w, float distance_delta_m, uint32_t stroke_delta) {
    if (!a || a->state != ACTIVITY_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    if (dt_s < 0) dt_s = 0;
    
    // Totals
    a->distance_m += distance_delta_m;
    a->stroke_count += stroke_delta;
    a->total_dt += dt_s;
    a->duration_ms = (uint32_t)(a->total_dt * 1000);

    // Accumulators for averages
    a->sum_speed_dt += speed_mps * dt_s;
    a->sum_spm_dt += spm * dt_s;
    a->sum_power_dt += power_w * dt_s;

    // Recompute Averages
    if (a->total_dt > 0.001f) {
        a->avg_speed_mps = (float)(a->sum_speed_dt / a->total_dt);
        a->avg_spm       = (float)(a->sum_spm_dt   / a->total_dt);
        a->avg_power_w   = (float)(a->sum_power_dt / a->total_dt);
    }
    
    // Update Max
    if (speed_mps > a->max_speed_mps) a->max_speed_mps = speed_mps;
    if (spm > a->max_spm) a->max_spm = spm;
    
    return ESP_OK;
}

esp_err_t activity_stop(activity_t *a, time_t end_ts) {
    if (!a || a->state != ACTIVITY_STATE_RECORDING) return ESP_ERR_INVALID_STATE;
    a->end_ts = (end_ts == 0) ? time(NULL) : end_ts;
    a->state = ACTIVITY_STATE_STOPPED;
    return ESP_OK;
}