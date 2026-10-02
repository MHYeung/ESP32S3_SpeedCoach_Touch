#include "ui_data_page.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "nvs_helper.h"
#include "ui_format.h"
#include "ui_theme.h"
#include "ui_typography.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DATA_SLOT_MAX 3

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_slot_box[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_title[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_value[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_unit[DATA_SLOT_MAX] = {0};

static lv_obj_t *s_activity_toast = NULL;
static lv_timer_t *s_activity_toast_timer = NULL;

static data_metric_t s_slot_metric[DATA_SLOT_MAX] = {
    DATA_METRIC_PACE,
    DATA_METRIC_SPM,
    DATA_METRIC_DISTANCE,
};

static data_values_t s_values = {0};
static bool s_recording = false;
static bool s_touch_locked = false;

/* Two rows in every orientation: major value on top, two secondaries below. */
static int32_t s_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static int32_t s_row_dsc[] = {LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};

static void persist_metrics(void)
{
    uint8_t raw[3] = {
        (uint8_t)s_slot_metric[0],
        (uint8_t)s_slot_metric[1],
        (uint8_t)s_slot_metric[2],
    };
    nvs_helper_set_data_metrics(raw);
}

static void metric_title_unit(data_metric_t metric, const char **title, const char **unit)
{
    switch (metric) {
    case DATA_METRIC_PACE:
        *title = "Pace";
        *unit = "/500m";
        break;
    case DATA_METRIC_AVG_PACE:
        *title = "Avg Pace";
        *unit = "/500m";
        break;
    case DATA_METRIC_TIME:
        *title = "Time";
        *unit = "";
        break;
    case DATA_METRIC_DISTANCE:
        *title = "Distance";
        *unit = "m";
        break;
    case DATA_METRIC_SPEED:
        *title = "Speed";
        *unit = "km/h";
        break;
    case DATA_METRIC_SPM:
        *title = "SPM";
        *unit = "";
        break;
    case DATA_METRIC_STROKE_LEN:
        *title = "Stroke Len";
        *unit = "m";
        break;
    case DATA_METRIC_STROKE_COUNT:
        *title = "Strokes";
        *unit = "";
        break;
    case DATA_METRIC_HR:
        *title = "HR";
        *unit = "bpm";
        break;
    case DATA_METRIC_CATCH:
        *title = "Catch";
        *unit = "deg";
        break;
    case DATA_METRIC_ARC:
        *title = "Arc";
        *unit = "deg";
        break;
    default:
        *title = "?";
        *unit = "";
        break;
    }
}

static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void apply_metric_to_slot(int idx)
{
    if (idx < 0 || idx >= DATA_SLOT_MAX) {
        return;
    }
    if (!s_slot_title[idx] || !s_slot_value[idx] || !s_slot_unit[idx]) {
        return;
    }

    data_metric_t metric = s_slot_metric[idx];
    const char *title = "?";
    const char *unit = "";
    metric_title_unit(metric, &title, &unit);

    char value_buf[32] = {0};
    const char *unit_override = NULL;

    switch (metric) {
    case DATA_METRIC_PACE:
        ui_fmt_pace_s(s_values.pace_s_per_500m, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_AVG_PACE:
        ui_fmt_pace_s(s_values.avg_pace_s_per_500m, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_TIME:
        ui_fmt_time_s(s_values.time_s, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_DISTANCE:
        ui_fmt_distance_m(s_values.distance_m, value_buf, sizeof(value_buf), &unit_override);
        break;
    case DATA_METRIC_SPEED:
        if (!isfinite(s_values.speed_mps) || s_values.speed_mps < 0.0f) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%.1f", (double)(s_values.speed_mps * 3.6f));
        }
        break;
    case DATA_METRIC_SPM:
        ui_fmt_spm(s_values.spm, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_STROKE_LEN:
        if (!isfinite(s_values.stroke_len_m) || s_values.stroke_len_m <= 0.0f) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%.1f", (double)s_values.stroke_len_m);
        }
        break;
    case DATA_METRIC_STROKE_COUNT:
        if (s_values.stroke_count == UINT32_MAX) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%lu", (unsigned long)s_values.stroke_count);
        }
        break;
    case DATA_METRIC_HR:
        if (s_values.hr_bpm == 0) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%u", (unsigned)s_values.hr_bpm);
        }
        break;
    case DATA_METRIC_CATCH:
        if (!s_values.pod_valid) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%.0f", (double)s_values.pod_catch_deg);
        }
        break;
    case DATA_METRIC_ARC:
        if (!s_values.pod_valid) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%.0f", (double)s_values.pod_arc_deg);
        }
        break;
    default:
        snprintf(value_buf, sizeof(value_buf), "--");
        break;
    }

    const char *unit_text = unit_override ? unit_override : unit;
    label_set_text_if_changed(s_slot_title[idx], title);
    label_set_text_if_changed(s_slot_value[idx], value_buf);
    label_set_text_if_changed(s_slot_unit[idx], unit_text);
}

static void slot_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_CLICKED) {
        return;
    }
    if (s_recording && s_touch_locked) {
        return;
    }

    uintptr_t idx_u = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)idx_u;
    if (idx < 0 || idx >= DATA_SLOT_MAX) {
        return;
    }

    s_slot_metric[idx] = (data_metric_t)((s_slot_metric[idx] + 1) % DATA_METRIC_COUNT);
    apply_metric_to_slot(idx);
    persist_metrics();
}

static void style_box(lv_obj_t *box)
{
    ui_theme_apply_surface(box);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);
}

static void build_slot(int idx)
{
    s_slot_box[idx] = lv_obj_create(s_root);
    style_box(s_slot_box[idx]);
    lv_obj_set_style_pad_all(s_slot_box[idx], 4, 0);

    lv_obj_add_event_cb(s_slot_box[idx], slot_event_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(s_slot_box[idx], slot_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    s_slot_title[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_title[idx], ui_font_caption(), 0);
    ui_theme_apply_label(s_slot_title[idx], true);
    lv_obj_align(s_slot_title[idx], LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_slot_title[idx], "?");

    s_slot_value[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_value[idx], idx == 0 ? ui_font_value_lg() : ui_font_value_sm(), 0);
    ui_theme_apply_label(s_slot_value[idx], false);
    lv_obj_align(s_slot_value[idx], LV_ALIGN_CENTER, 0, 4);
    lv_label_set_text(s_slot_value[idx], "--");

    s_slot_unit[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_unit[idx], ui_font_caption(), 0);
    ui_theme_apply_label(s_slot_unit[idx], true);
    lv_obj_align(s_slot_unit[idx], LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(s_slot_unit[idx], "");
}

static void apply_layout(void)
{
    lv_obj_set_layout(s_root, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_style_pad_row(s_root, 4, 0);
    lv_obj_set_style_pad_column(s_root, 4, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);

    lv_obj_set_style_text_font(s_slot_value[0], ui_font_value_lg(), 0);
    lv_obj_set_style_text_font(s_slot_value[1], ui_font_value_sm(), 0);
    lv_obj_set_style_text_font(s_slot_value[2], ui_font_value_sm(), 0);

    lv_obj_set_grid_dsc_array(s_root, s_col_dsc, s_row_dsc);
    lv_obj_set_grid_cell(s_slot_box[0], LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_grid_cell(s_slot_box[1], LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_grid_cell(s_slot_box[2], LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
}

static void activity_toast_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (s_activity_toast) {
        lv_obj_del(s_activity_toast);
        s_activity_toast = NULL;
    }
    if (s_activity_toast_timer) {
        lv_timer_del(s_activity_toast_timer);
        s_activity_toast_timer = NULL;
    }
}

static void activity_toast_create_async(void *param)
{
    bool recording = (bool)(uintptr_t)param;
    if (s_activity_toast) {
        lv_obj_del(s_activity_toast);
        s_activity_toast = NULL;
    }
    if (s_activity_toast_timer) {
        lv_timer_del(s_activity_toast_timer);
        s_activity_toast_timer = NULL;
    }

    lv_obj_t *top = lv_layer_top();
    s_activity_toast = lv_obj_create(top);
    lv_obj_set_size(s_activity_toast, 90, 90);
    lv_obj_set_style_radius(s_activity_toast, 45, 0);
    lv_obj_set_style_border_width(s_activity_toast, 0, 0);
    lv_obj_set_style_bg_opa(s_activity_toast, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_activity_toast,
                              recording ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED),
                              0);
    lv_obj_clear_flag(s_activity_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_activity_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_activity_toast);

    lv_obj_t *lbl = lv_label_create(s_activity_toast);
    lv_label_set_text(lbl, recording ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(lbl, ui_font_symbol(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    s_activity_toast_timer = lv_timer_create(activity_toast_hide_cb, 1200, NULL);
}

void data_page_show_activity_toast(bool recording)
{
    lv_async_call(activity_toast_create_async, (void *)(uintptr_t)recording);
}

void data_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_root, LV_DIR_NONE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);

    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        build_slot(i);
    }

    apply_layout();
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }
}

void data_page_apply_theme(void)
{
    if (!s_root) {
        return;
    }
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        if (s_slot_box[i]) {
            style_box(s_slot_box[i]);
        }
        if (s_slot_title[i]) {
            ui_theme_apply_label(s_slot_title[i], true);
        }
        if (s_slot_value[i]) {
            ui_theme_apply_label(s_slot_value[i], false);
        }
        if (s_slot_unit[i]) {
            ui_theme_apply_label(s_slot_unit[i], true);
        }
    }
}

void data_page_set_orientation(ui_orientation_t o)
{
    (void)o;
    if (s_root) {
        apply_layout();
    }
}

void data_page_set_metrics(const data_metric_t metrics[], size_t count)
{
    if (!metrics || count == 0) {
        return;
    }
    size_t n = count;
    if (n > DATA_SLOT_MAX) {
        n = DATA_SLOT_MAX;
    }
    lvgl_port_lock(0);
    for (size_t i = 0; i < n; i++) {
        if (metrics[i] < DATA_METRIC_COUNT) {
            s_slot_metric[i] = metrics[i];
        }
    }
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }
    lvgl_port_unlock();
}

void data_page_set_time_s(float time_s)
{
    s_values.time_s = time_s;
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        if (s_slot_metric[i] == DATA_METRIC_TIME) {
            apply_metric_to_slot(i);
        }
    }
}

void data_page_set_values(const data_values_t *v)
{
    if (!v) {
        return;
    }
    s_values = *v;
    lvgl_port_lock(0);
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }
    lvgl_port_unlock();
}

void data_page_apply_snapshot(const coach_ui_snapshot_t *snap)
{
    if (!snap || !s_root) {
        return;
    }
    s_recording = snap->recording;
    s_touch_locked = snap->touch_locked;
    s_values.time_s = snap->time_s;
    s_values.distance_m = snap->distance_m;
    s_values.pace_s_per_500m = snap->pace_s_per_500m;
    s_values.avg_pace_s_per_500m = snap->avg_pace_s_per_500m;
    s_values.speed_mps = snap->speed_mps;
    s_values.spm = snap->spm;
    s_values.stroke_len_m = snap->stroke_len_m;
    s_values.stroke_count = snap->stroke_count;
    s_values.hr_bpm = snap->hr_bpm;
    s_values.pod_valid = snap->pod_valid;
    s_values.pod_catch_deg = snap->pod_catch_deg;
    s_values.pod_arc_deg = snap->pod_arc_deg;
    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }
}
