// main/ui/ui_data_page.h
#pragma once
#include <stddef.h>
#include "ui.h"

typedef enum {
    DATA_METRIC_PACE,
    DATA_METRIC_TIME,
    DATA_METRIC_DISTANCE,
    DATA_METRIC_SPEED,
    DATA_METRIC_SPM,
    DATA_METRIC_POWER,
    DATA_METRIC_COUNT
} data_metric_t;

typedef struct {
    float time_s;          // total elapsed
    float distance_m;
    float pace_s_per_500m;
    float speed_mps;
    float spm;
    float power_w;
} data_values_t;

void data_page_create(lv_obj_t *parent);
void data_page_set_orientation(ui_orientation_t o);
void data_page_set_metrics(const data_metric_t metrics[], size_t count); // per slot
void data_page_set_values(const data_values_t *v);
