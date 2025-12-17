// main/ui/ui_settings_page.c
#include "ui_settings_page.h"
#include "ui.h"       // for ui_notify_* functions
#include "ui_theme.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers to build rows                                                      */
/* -------------------------------------------------------------------------- */

static lv_obj_t *create_settings_row(lv_obj_t *parent,
                                     const char *label_txt,
                                     lv_event_cb_t switch_event_cb,
                                     bool initial_state)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    ui_theme_apply_surface(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    ui_theme_apply_switch(sw);
    lv_obj_add_event_cb(sw, switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    return sw;
}

/* Row with a value label on the right (for device name, status, etc.) */
static lv_obj_t *create_value_row(lv_obj_t *parent,
                                  const char *label_txt,
                                  const char *value_txt)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    ui_theme_apply_surface(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    if (value_txt) {
        lv_label_set_text(val, value_txt);
    } else {
        lv_label_set_text(val, "");
    }
    ui_theme_apply_label(val, true);

    return val; // value-label, so caller can update text later
}

/* -------------------------------------------------------------------------- */
/* Switch event handlers – notify ui_core                                     */
/* -------------------------------------------------------------------------- */

static void sw_dark_mode_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);   // LVGL 9.x API
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_notify_dark_mode_changed(on);
}

static void sw_auto_rotate_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);   // LVGL 9.x API
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_notify_auto_rotate_changed(on);
}

/* labels & widgets in main settings page */
static lv_obj_t *s_root                    = NULL;
static lv_obj_t *s_header                  = NULL;
static lv_obj_t *s_body                    = NULL;
static lv_obj_t *s_time_label              = NULL;
static lv_obj_t *s_gps_label               = NULL;
static lv_obj_t *s_batt_label              = NULL;
static lv_timer_t *s_clock_timer           = NULL;
static uint32_t s_clock_start_ms           = 0;
static uint32_t s_clock_start_sec          = 12 * 3600; /* Dummy start: 12:00:00 */

static lv_obj_t *s_dark_mode_sw            = NULL;

static lv_obj_t *device_name_value_label   = NULL;

static bool s_header_swipe_armed = false;
static lv_point_t s_header_swipe_sum = {0};

static void settings_header_swipe_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        s_header_swipe_sum.x = 0;
        s_header_swipe_sum.y = 0;
        s_header_swipe_armed = true;
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        s_header_swipe_armed = false;
        return;
    }

    if (code != LV_EVENT_PRESSING) return;
    if (!s_header_swipe_armed) return;

    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    s_header_swipe_sum.x += v.x;
    s_header_swipe_sum.y += v.y;

    const int32_t min_px = 30;
    if (s_header_swipe_sum.y > -min_px) return;
    if (LV_ABS(s_header_swipe_sum.y) < (LV_ABS(s_header_swipe_sum.x) + 10)) return;

    s_header_swipe_armed = false;
    lv_indev_stop_processing(indev);
    lv_indev_wait_release(indev);
    ui_go_to_page(UI_PAGE_DATA, true);
}

static void settings_clock_update(void)
{
    if (!s_time_label) return;

    uint32_t now_ms = lv_tick_get();
    uint32_t elapsed_s = (now_ms - s_clock_start_ms) / 1000;
    uint32_t t = s_clock_start_sec + elapsed_s;

    uint32_t hh = (t / 3600) % 24;
    uint32_t mm = (t / 60) % 60;
    uint32_t ss = t % 60;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)hh, (unsigned)mm, (unsigned)ss);
    lv_label_set_text(s_time_label, buf);
}

static void settings_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    settings_clock_update();
}

void settings_page_set_dark_mode_state(bool enabled)
{
    if (!s_dark_mode_sw) return;

    if (enabled) lv_obj_add_state(s_dark_mode_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_dark_mode_sw, LV_STATE_CHECKED);
}

void settings_page_set_gps_status(bool connected, uint8_t bars_0_to_4)
{
    if (!s_gps_label) return;

    if (!connected) {
        lv_label_set_text(s_gps_label, LV_SYMBOL_GPS " " LV_SYMBOL_CLOSE);
        return;
    }

    uint8_t bars = bars_0_to_4;
    if (bars > 4) bars = 4;

    if (bars == 0) {
        lv_label_set_text(s_gps_label, LV_SYMBOL_GPS " " LV_SYMBOL_WARNING);
        return;
    }

    char bars_txt[5];
    for (uint8_t i = 0; i < bars; i++) {
        bars_txt[i] = '|';
    }
    bars_txt[bars] = '\0';

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s%s", LV_SYMBOL_GPS, LV_SYMBOL_WIFI, bars_txt);
    lv_label_set_text(s_gps_label, buf);
}

/* -------------------------------------------------------------------------- */
/* Settings page entry point                                                  */
/* -------------------------------------------------------------------------- */

void settings_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_center(s_root);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);

    /* Header/status bar (top ~10%) */
    s_header = lv_obj_create(s_root);
    ui_theme_apply_surface(s_header);
    lv_obj_set_width(s_header, lv_pct(100));
    lv_obj_set_height(s_header, lv_pct(10));
    lv_obj_set_style_radius(s_header, 0, 0);
    lv_obj_set_style_border_width(s_header, 0, 0);
    lv_obj_set_style_pad_hor(s_header, 10, 0);
    lv_obj_set_style_pad_ver(s_header, 6, 0);
    lv_obj_add_flag(s_header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_header, settings_header_swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_header, settings_header_swipe_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_header, settings_header_swipe_cb, LV_EVENT_RELEASED, NULL);

    /* 3-column grid: time | GPS | battery */
    static int32_t s_hdr_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t s_hdr_rows[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_layout(s_header, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(s_header, s_hdr_cols, s_hdr_rows);

    s_time_label = lv_label_create(s_header);
    ui_theme_apply_label(s_time_label, false);
    lv_obj_add_flag(s_time_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_grid_cell(s_time_label, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    s_gps_label = lv_label_create(s_header);
    lv_label_set_text(s_gps_label, "");
    ui_theme_apply_label(s_gps_label, true);
    lv_obj_add_flag(s_gps_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_align(s_gps_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_gps_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_grid_cell(s_gps_label, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    settings_page_set_gps_status(false, 0);

    s_batt_label = lv_label_create(s_header);
    lv_label_set_text(s_batt_label, "BAT: --%");
    ui_theme_apply_label(s_batt_label, true);
    lv_obj_add_flag(s_batt_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(s_batt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_batt_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_grid_cell(s_batt_label, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    s_clock_start_ms = lv_tick_get();
    if (!s_clock_timer) {
        s_clock_timer = lv_timer_create(settings_clock_timer_cb, 1000, NULL);
    }
    settings_clock_update();

    /* Body (remaining ~90%) */
    s_body = lv_obj_create(s_root);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_body, 10, 0);
    lv_obj_set_style_pad_row(s_body, 10, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);

    /* Dark mode row */
    s_dark_mode_sw = create_settings_row(s_body,
                                         "Dark mode",
                                         sw_dark_mode_event_cb,
                                         ui_get_dark_mode());
    settings_page_set_dark_mode_state(ui_get_dark_mode());

    /* Auto rotate row */
    create_settings_row(s_body,
                        "Auto rotate",
                        sw_auto_rotate_event_cb,
                        true);

    /* Device name row (static for now, must match ble.c name) */
    device_name_value_label = create_value_row(s_body,
                                               "Device",
                                               "ESP32S3-BLE");
}

void settings_page_apply_theme(void)
{
    if (s_root) {
        lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_root, 0, 0);
    }

    if (s_header) {
        ui_theme_apply_surface(s_header);
        lv_obj_set_style_radius(s_header, 0, 0);
        lv_obj_set_style_border_width(s_header, 0, 0);
    }
    if (s_time_label) ui_theme_apply_label(s_time_label, false);
    if (s_gps_label) ui_theme_apply_label(s_gps_label, true);
    if (s_batt_label) ui_theme_apply_label(s_batt_label, true);

    if (s_body) {
        lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_body, 0, 0);
    }

    if (device_name_value_label) ui_theme_apply_label(device_name_value_label, true);
    settings_page_set_dark_mode_state(ui_get_dark_mode());
}
