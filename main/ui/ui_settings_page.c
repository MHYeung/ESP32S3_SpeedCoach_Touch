// main/ui/ui_settings_page.c
#include "ui_settings_page.h"
#include "ui.h"       // for ui_notify_* functions
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "rtc_pcf85063.h"

#include <stdbool.h>
#include <stdint.h>

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
static lv_obj_t *s_body                    = NULL;
static ui_status_bar_t s_status            = {0};

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

void settings_page_set_dark_mode_state(bool enabled)
{
    if (!s_dark_mode_sw) return;

    if (enabled) lv_obj_add_state(s_dark_mode_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_dark_mode_sw, LV_STATE_CHECKED);
}

void settings_page_set_gps_status(bool connected, uint8_t bars_0_to_4)
{
    ui_status_bar_set_gps_status(&s_status, connected, bars_0_to_4);
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
    ui_status_bar_create(&s_status, s_root);
    lv_obj_t *header = ui_status_bar_root(&s_status);
    if (header) {
        lv_obj_add_event_cb(header, settings_header_swipe_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(header, settings_header_swipe_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(header, settings_header_swipe_cb, LV_EVENT_RELEASED, NULL);
    }
    settings_page_set_gps_status(false, 0);

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

    ui_status_bar_apply_theme(&s_status);

    if (s_body) {
        lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_body, 0, 0);
    }

    if (device_name_value_label) ui_theme_apply_label(device_name_value_label, true);
    settings_page_set_dark_mode_state(ui_get_dark_mode());
}
