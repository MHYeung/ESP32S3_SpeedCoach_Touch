#pragma once

#include "lvgl.h"
#include "ui.h"
#include "coach_ui_snapshot.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_STATUS_BAR_FULL = 0,
    UI_STATUS_BAR_COMPACT,
} ui_status_bar_kind_t;

#define UI_STATUS_BAR_COMPACT_H 22
#define UI_STATUS_BAR_FULL_H    20

typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_obj_t *title_btn;
    lv_obj_t *back_icon;
    lv_obj_t *title_label;
    ui_page_t back_page;
    bool has_back;

    lv_obj_t *gps_cont;
    lv_obj_t *gps_icon;
    lv_obj_t *gps_bars[4];

    lv_obj_t *batt_label;
    lv_obj_t *rec_dot;
    lv_obj_t *lock_label;
    lv_obj_t *ble_label;

    ui_orientation_t orient;
    ui_status_bar_kind_t kind;
    bool gps_connected;
    uint8_t gps_bars_count;
    bool gps_stale;
    bool recording;
    bool touch_locked;
    sensor_link_t sensor_link;
} ui_status_bar_t;

void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent);
/** Menu-type rail: title plus a back chevron. Tapping it opens back_page. */
void ui_status_bar_set_title(ui_status_bar_t *bar, const char *title, ui_page_t back_page);
void ui_status_bar_create_ex(ui_status_bar_t *bar, lv_obj_t *parent, ui_status_bar_kind_t kind);
void ui_status_bar_apply_theme(ui_status_bar_t *bar);
void ui_status_bar_set_orientation(ui_status_bar_t *bar, ui_orientation_t o);
void ui_status_bar_set_gps_status(ui_status_bar_t *bar, bool connected, uint8_t bars_0_to_4);
void ui_status_bar_set_battery(ui_status_bar_t *bar, int percent);
void ui_status_bar_set_time_base(ui_status_bar_t *bar, uint32_t start_sec);
lv_obj_t *ui_status_bar_root(ui_status_bar_t *bar);

void ui_status_bar_force_refresh(ui_status_bar_t *bar);

/** Any task: cache GPS and push to every registered bar on the LVGL thread. */
void ui_status_bar_set_gps_default_safe(bool connected, uint8_t bars_0_to_4);
void ui_status_bar_set_default(ui_status_bar_t *bar);

void ui_status_bar_apply_snapshot(const coach_ui_snapshot_t *snap);
