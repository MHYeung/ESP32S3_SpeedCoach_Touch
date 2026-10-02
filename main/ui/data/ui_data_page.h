#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui.h"
#include "coach_ui_snapshot.h"

typedef enum {
    DATA_METRIC_PACE,
    DATA_METRIC_AVG_PACE,
    DATA_METRIC_TIME,
    DATA_METRIC_DISTANCE,
    DATA_METRIC_SPEED,
    DATA_METRIC_SPM,
    DATA_METRIC_STROKE_LEN,
    DATA_METRIC_STROKE_COUNT,
    DATA_METRIC_HR,
    DATA_METRIC_CATCH,
    DATA_METRIC_ARC,
    DATA_METRIC_COUNT
} data_metric_t;

typedef struct {
    float time_s;
    float distance_m;
    float pace_s_per_500m;
    float avg_pace_s_per_500m;
    float speed_mps;
    float spm;
    float stroke_len_m;
    uint32_t stroke_count;
    uint16_t hr_bpm;
    bool pod_valid;
    float pod_catch_deg;
    float pod_arc_deg;
} data_values_t;

void data_page_create(lv_obj_t *parent);
void data_page_set_orientation(ui_orientation_t o);
void data_page_set_metrics(const data_metric_t metrics[], size_t count);

void data_page_set_time_s(float time_s); /* LVGL thread only */
void data_page_set_values(const data_values_t *v);
void data_page_apply_snapshot(const coach_ui_snapshot_t *snap);
void data_page_apply_theme(void);

void data_page_show_activity_toast(bool recording);
