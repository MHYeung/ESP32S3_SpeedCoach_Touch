// main/ui/ui_settings_page.c
#include "ui_settings_page.h"
#include "ui.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static lv_obj_t *create_settings_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t switch_event_cb, bool initial_state)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 8, 0); // Standard padding
    lv_obj_set_style_border_width(row, 0, 0);
    ui_theme_apply_surface(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    if (initial_state) lv_obj_add_state(sw, LV_STATE_CHECKED);
    ui_theme_apply_switch(sw);
    lv_obj_add_event_cb(sw, switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    return sw;
}

static lv_obj_t *create_value_row(lv_obj_t *parent, const char *label_txt, const char *value_txt)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    ui_theme_apply_surface(row);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value_txt ? value_txt : "");
    ui_theme_apply_label(val, true);

    return val;
}

/* -------------------------------------------------------------------------- */
/* Event Callbacks                                                            */
/* -------------------------------------------------------------------------- */

static void sw_dark_mode_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_notify_dark_mode_changed(on);
}

static void sw_auto_rotate_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_notify_auto_rotate_changed(on);
}

static void settings_header_swipe_cb(lv_event_t *e)
{
    // Pass event to parent gesture handler if needed, or handle header-specific swipe here
    // Currently relying on ui_core's gesture layers, but keeping this hook if specific behavior needed
}

/* -------------------------------------------------------------------------- */
/* Main Creation                                                              */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_body = NULL;
static ui_status_bar_t s_status = {0};
static lv_obj_t *s_dark_mode_sw = NULL;
static lv_obj_t *s_device_lbl = NULL;

void settings_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    /* 1. Status Bar */
    ui_status_bar_create(&s_status, s_root);
    
    // Optional: make status bar swipable for gestures
    lv_obj_t *header = ui_status_bar_root(&s_status);
    if (header) lv_obj_add_event_cb(header, settings_header_swipe_cb, LV_EVENT_ALL, NULL);
    
    settings_page_set_gps_status(false, 0);

    /* 2. Body */
    s_body = lv_obj_create(s_root);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1); // Fill remaining height
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_body, 10, 0);
    lv_obj_set_style_pad_row(s_body, 10, 0);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    /* 3. Rows */
    s_dark_mode_sw = create_settings_row(s_body, "Dark Mode", sw_dark_mode_event_cb, ui_get_dark_mode());
    create_settings_row(s_body, "Auto Rotate", sw_auto_rotate_event_cb, true);
    s_device_lbl = create_value_row(s_body, "Device", "ESP32S3-BLE");
}

void settings_page_apply_theme(void)
{
    if (s_root) ui_status_bar_apply_theme(&s_status);
    if (s_device_lbl) ui_theme_apply_label(s_device_lbl, true);
    settings_page_set_dark_mode_state(ui_get_dark_mode());
}

void settings_page_set_gps_status(bool connected, uint8_t bars)
{
    ui_status_bar_set_gps_status(&s_status, connected, bars);
}

void settings_page_set_dark_mode_state(bool enabled)
{
    if (!s_dark_mode_sw) return;
    if (enabled) lv_obj_add_state(s_dark_mode_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_dark_mode_sw, LV_STATE_CHECKED);
}

void settings_page_on_orientation_changed(void)
{
    ui_status_bar_force_refresh(&s_status);
}