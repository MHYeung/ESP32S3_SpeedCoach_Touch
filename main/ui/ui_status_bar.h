#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_obj_t *gps_label;
    lv_obj_t *batt_label;
    lv_timer_t *clock_timer;
    uint32_t clock_start_ms;
    uint32_t clock_start_sec;
} ui_status_bar_t;

void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent);
void ui_status_bar_apply_theme(ui_status_bar_t *bar);
void ui_status_bar_set_gps_status(ui_status_bar_t *bar, bool connected, uint8_t bars_0_to_4);
void ui_status_bar_set_battery_text(ui_status_bar_t *bar, const char *text);
void ui_status_bar_set_time_base(ui_status_bar_t *bar, uint32_t start_sec);
lv_obj_t *ui_status_bar_root(ui_status_bar_t *bar);
