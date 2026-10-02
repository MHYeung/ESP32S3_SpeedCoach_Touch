// main/ui/ui_settings_page.c
#include "ui_settings_page.h"
#include "ui.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "ui_typography.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app/app_usb_msc.h"
#include "app/app_context.h"
#include "nvs_helper.h"
#include "rtc_pcf85063.h"
#include "sensor_hub.h"
#include "ui_sensors_page.h"
#include "esp_err.h"
#include "esp_log.h"

/* -------------------------------------------------------------------------- */
/* State & Handles                                                            */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_body = NULL;
static ui_status_bar_t s_status = {0};
static lv_obj_t *s_dark_mode_sw = NULL;
static lv_obj_t *s_usb_drive_sw = NULL;
static lv_obj_t *s_usb_status_lbl = NULL;
static lv_obj_t *s_split_val_lbl = NULL;
static lv_obj_t *s_sensors_val_lbl = NULL;
static lv_obj_t *s_datetime_lbl = NULL;
static lv_obj_t *s_bright_lbl = NULL;
static lv_timer_t *s_datetime_timer = NULL;

// Default split is 1000m until changed
//static uint32_t s_current_split_m = 1000;
static ui_split_length_cb_t s_split_cb = NULL;

/* Dialog Handles */
static lv_obj_t *s_split_overlay = NULL;
static lv_obj_t *s_split_roller = NULL;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static void update_split_label_text(void)
{
    if (!s_split_val_lbl)
        return;
    char buf[32];
    if (s_current_split_m >= 1000)
    {
        snprintf(buf, sizeof(buf), "%.1f km", s_current_split_m / 1000.0f);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%d m", (int)s_current_split_m);
    }
    lv_label_set_text(s_split_val_lbl, buf);
}

static void group_row_chrome(lv_obj_t *row, bool divider)
{
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_border_width(row, divider ? 1 : 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, ui_theme_palette()->border, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *section_group(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, caption);
    ui_theme_apply_label(cap, true);
    lv_obj_set_style_text_font(cap, ui_font_caption(), 0);
    lv_obj_set_style_pad_left(cap, 4, 0);
    lv_obj_set_style_pad_top(cap, 8, 0);

    lv_obj_t *group = lv_obj_create(parent);
    ui_theme_apply_list_group(group);
    return group;
}

static lv_obj_t *create_clickable_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t click_cb, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_height(row, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    group_row_chrome(row, divider);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_style_text_font(lbl, ui_font_caption(), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    ui_theme_apply_label(val, true);
    lv_obj_set_style_text_font(val, ui_font_caption(), 0);
    lv_label_set_text(val, "");

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(icon, ui_theme_color_accent(), 0);
    lv_obj_set_style_pad_left(icon, 4, 0);

    return val;
}

static lv_obj_t *create_settings_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t switch_event_cb,
                                    bool initial_state, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_height(row, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    group_row_chrome(row, divider);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_style_text_font(lbl, ui_font_caption(), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    if (initial_state)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    ui_theme_apply_switch(sw);
    lv_obj_add_event_cb(sw, switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *create_value_row(lv_obj_t *parent, const char *label_txt, const char *value_txt, bool divider)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_height(row, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    group_row_chrome(row, divider);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    ui_theme_apply_label(lbl, false);
    lv_obj_set_style_text_font(lbl, ui_font_caption(), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value_txt ? value_txt : "");
    ui_theme_apply_label(val, true);
    lv_obj_set_style_text_font(val, ui_font_caption(), 0);
    return val;
}

static void update_datetime_label_text(void)
{
    if (!s_datetime_lbl)
        return;

    bool valid = false;
    if (PCF85063_is_time_valid(&valid) == ESP_OK && valid)
    {
        datetime_t rtc = {0};
        if (PCF85063_read_time(&rtc) == ESP_OK)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                     (unsigned)rtc.year, (unsigned)rtc.month, (unsigned)rtc.day);
            lv_label_set_text(s_datetime_lbl, buf);
            return;
        }
    }

    lv_label_set_text(s_datetime_lbl, "---- -- --");
}

static void datetime_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_datetime_label_text();
}

/* -------------------------------------------------------------------------- */
/* Split Dialog Logic                                                         */
/* -------------------------------------------------------------------------- */

static const uint32_t SPLIT_OPTIONS_M[] = {100, 250, 500, 750, 1000, 2000};
static const char *SPLIT_OPTIONS_STR = "100 m\n250 m\n500 m\n750 m\n1000 m\n2000 m";

static void split_dialog_event_cb(lv_event_t *e)
{
    const char *action = (const char *)lv_event_get_user_data(e);

    if (strcmp(action, "save") == 0 && s_split_roller)
    {
        uint16_t idx = lv_roller_get_selected(s_split_roller);
        if (idx < 6)
        {
            s_current_split_m = SPLIT_OPTIONS_M[idx];
            update_split_label_text();

            nvs_helper_set_split_len(s_current_split_m);
            // Notify Backend
            if (s_split_cb)
                s_split_cb(s_current_split_m);
        }
    }

    if (s_split_overlay)
    {
        lv_obj_del(s_split_overlay);
        s_split_overlay = NULL;
        s_split_roller = NULL;
    }
}

static void create_split_dialog(void)
{
    if (s_split_overlay)
        return;

    lv_obj_t *top = lv_layer_top();
    s_split_overlay = lv_obj_create(top);
    lv_obj_set_size(s_split_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_split_overlay, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_split_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_split_overlay, 0, 0);
    lv_obj_set_flex_flow(s_split_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_split_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *panel = lv_obj_create(s_split_overlay);
    ui_theme_apply_surface(panel);
    lv_obj_set_width(panel, 240);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 15, 0);
    lv_obj_set_style_pad_row(panel, 15, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Select Split");
    ui_theme_apply_label(title, false);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    s_split_roller = lv_roller_create(panel);
    lv_roller_set_options(s_split_roller, SPLIT_OPTIONS_STR, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_split_roller, 3);
    lv_obj_set_width(s_split_roller, lv_pct(80));
    lv_obj_center(s_split_roller);

    // Set selection
    for (int i = 0; i < 6; i++)
    {
        if (SPLIT_OPTIONS_M[i] == s_current_split_m)
        {
            lv_roller_set_selected(s_split_roller, i, LV_ANIM_OFF);
            break;
        }
    }

    lv_obj_t *btns = lv_obj_create(panel);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_save = lv_btn_create(btns);
    lv_obj_set_width(btn_save, lv_pct(47));
    ui_theme_apply_button(btn_save);
    lv_obj_add_event_cb(btn_save, split_dialog_event_cb, LV_EVENT_CLICKED, (void *)"save");
    lv_obj_t *l1 = lv_label_create(btn_save);
    lv_label_set_text(l1, "OK");
    lv_obj_center(l1);

    lv_obj_t *btn_cancel = lv_btn_create(btns);
    lv_obj_set_width(btn_cancel, lv_pct(47));
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x6B7280), 0);
    lv_obj_add_event_cb(btn_cancel, split_dialog_event_cb, LV_EVENT_CLICKED, (void *)"cancel");
    lv_obj_t *l2 = lv_label_create(btn_cancel);
    lv_label_set_text(l2, "Cancel");
    lv_obj_center(l2);
}

static void split_row_click_cb(lv_event_t *e) { create_split_dialog(); }

static void update_sensors_label(void)
{
    if (!s_sensors_val_lbl)
        return;
    if (!sensor_hub_available()) {
        lv_label_set_text(s_sensors_val_lbl, "Off");
        return;
    }
    sensor_hub_slot_info_t hr;
    sensor_hub_slot_info_t pod;
    sensor_hub_get_slot(SENSOR_KIND_HR, &hr);
    sensor_hub_get_slot(SENSOR_KIND_ROWPOD, &pod);
    char buf[32];
    snprintf(buf, sizeof(buf), "HR %s / Pod %s",
             hr.state == SENSOR_HUB_CONNECTED ? "On" : "Off",
             pod.state == SENSOR_HUB_CONNECTED ? "On" : "Off");
    lv_label_set_text(s_sensors_val_lbl, buf);
}

static void sensors_row_click_cb(lv_event_t *e)
{
    (void)e;
    sensors_page_set_return_page(UI_SETTINGS_PAGE);
    ui_go_to_page(UI_SENSORS_PAGE, true);
}

/* -------------------------------------------------------------------------- */
/* Callbacks                                                                  */
/* -------------------------------------------------------------------------- */

static void sw_dark_mode_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    ui_notify_dark_mode_changed(on);
    nvs_helper_set_dark_mode(on);
}

static void usb_status_set(const char *msg)
{
    if (s_usb_status_lbl)
        lv_label_set_text(s_usb_status_lbl, msg ? msg : "");
}

static void usb_switch_show(bool on)
{
    if (!s_usb_drive_sw)
        return;
    if (on)
        lv_obj_add_state(s_usb_drive_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(s_usb_drive_sw, LV_STATE_CHECKED);
}

static void usb_drive_row_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    bool want_on = !app_usb_msc_is_active();

    if (want_on && s_activity_recording)
    {
        usb_status_set("Stop the activity first");
        ESP_LOGW("UI", "Cannot enable USB drive while recording");
        return;
    }
    if (want_on && !s_sd.mounted)
    {
        usb_status_set("No SD card mounted");
        ESP_LOGW("UI", "Cannot enable USB drive: SD not mounted");
        return;
    }

    /* Mount/unmount runs on a worker task, so move the knob now and let
     * settings_page_sync_usb_state() correct it when the worker reports back. */
    usb_switch_show(want_on);
    usb_status_set(want_on ? "Starting USB drive..." : "Stopping USB drive...");

    esp_err_t ret = want_on ? app_usb_msc_request_enter() : app_usb_msc_request_leave();
    if (ret != ESP_OK)
    {
        usb_switch_show(!want_on);
        if (ret == ESP_ERR_NO_MEM)
            usb_status_set("Not enough RAM to start USB");
        else if (ret == ESP_ERR_INVALID_STATE)
            usb_status_set("Busy, try again");
        else
            usb_status_set("Request failed");
        ESP_LOGE("UI", "USB MSC request failed: %s", esp_err_to_name(ret));
    }
}

static void sw_auto_rotate_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool auto_rot_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    // 1. Update UI & NVS for the Toggle
    ui_notify_auto_rotate_changed(auto_rot_on);
    nvs_helper_set_auto_rotate(auto_rot_on);

    // 2. IF LOCKING (Turning OFF) -> Save Current Orientation
    if (!auto_rot_on)
    {
        // Get current system rotation
        lv_display_rotation_t current_rot = lv_display_get_rotation(NULL); // NULL = default display

        // Map LVGL rotation -> Your Custom Enum
        ui_orientation_t save_orient = UI_ORIENT_PORTRAIT_0; // Default

        switch (current_rot)
        {
        case LV_DISPLAY_ROTATION_0:
            save_orient = UI_ORIENT_PORTRAIT_0;
            break;
        case LV_DISPLAY_ROTATION_90:
            save_orient = UI_ORIENT_LANDSCAPE_90;
            break;
        case LV_DISPLAY_ROTATION_180:
            save_orient = UI_ORIENT_PORTRAIT_180;
            break;
        case LV_DISPLAY_ROTATION_270:
            save_orient = UI_ORIENT_LANDSCAPE_270;
            break;
        }

        ESP_LOGI("UI", "Locking Orientation: %d", save_orient);

        // Save as uint8_t
        nvs_helper_set_orientation((uint8_t)save_orient); //
    }
}

static void sw_auto_dim_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_set_auto_dim(on);
    nvs_helper_set_auto_dim(on);
}

static void sw_metrics_lock_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    nvs_helper_set_metrics_lock(on);
}

static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target_obj(e);
    int32_t v = lv_slider_get_value(sl);
    ui_set_brightness_percent((uint8_t)v);
    nvs_helper_set_brightness((uint8_t)v);
    if (s_bright_lbl) {
        lv_label_set_text_fmt(s_bright_lbl, "%d%%", (int)v);
    }
}

/* -------------------------------------------------------------------------- */
/* Main Creation                                                              */
/* -------------------------------------------------------------------------- */

void settings_page_create(lv_obj_t *parent)
{
    bool is_dark = nvs_helper_get_dark_mode();
    bool is_rot = nvs_helper_get_auto_rotate();
    s_current_split_m = nvs_helper_get_split_len();

    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    ui_status_bar_create(&s_status, s_root);
    ui_status_bar_set_title(&s_status, "Settings", UI_PAGE_MENU);
    settings_page_set_gps_status(false, 0);

    s_body = lv_obj_create(s_root);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_body, 8, 0);
    lv_obj_set_style_pad_row(s_body, 2, 0);
    lv_obj_set_style_pad_bottom(s_body, 28, 0);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    {
        lv_obj_t *display = section_group(s_body, "Display");
        s_dark_mode_sw = create_settings_row(display, "Dark mode", sw_dark_mode_event_cb, is_dark, true);
        create_settings_row(display, "Auto rotate", sw_auto_rotate_event_cb, is_rot, true);
        create_settings_row(display, "Auto-dim", sw_auto_dim_event_cb, nvs_helper_get_auto_dim(), true);

        lv_obj_t *bright = lv_obj_create(display);
        lv_obj_set_height(bright, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(bright, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(bright, 2, 0);
        group_row_chrome(bright, false);
        lv_obj_set_style_pad_ver(bright, 8, 0);

        lv_obj_t *hdr = lv_obj_create(bright);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_width(hdr, lv_pct(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *lbl = lv_label_create(hdr);
        lv_label_set_text(lbl, "Brightness");
        ui_theme_apply_label(lbl, false);
        lv_obj_set_style_text_font(lbl, ui_font_caption(), 0);

        s_bright_lbl = lv_label_create(hdr);
        ui_theme_apply_label(s_bright_lbl, true);
        lv_obj_set_style_text_font(s_bright_lbl, ui_font_caption(), 0);
        lv_label_set_text_fmt(s_bright_lbl, "%d%%", (int)nvs_helper_get_brightness());

        lv_obj_t *sl = lv_slider_create(bright);
        lv_obj_set_width(sl, lv_pct(100));
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, nvs_helper_get_brightness(), LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *session = section_group(s_body, "Session");
        create_settings_row(session, "Lock metrics", sw_metrics_lock_event_cb, nvs_helper_get_metrics_lock(), true);
        s_split_val_lbl = create_clickable_row(session, "Split length", split_row_click_cb, false);
        update_split_label_text();
    }

    {
        lv_obj_t *links = section_group(s_body, "Connections");
        s_sensors_val_lbl = create_clickable_row(links, "Sensors", sensors_row_click_cb, true);

        lv_obj_t *usb = lv_obj_create(links);
        lv_obj_set_height(usb, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(usb, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(usb, 2, 0);
        group_row_chrome(usb, false);
        lv_obj_set_style_pad_ver(usb, 6, 0);
        lv_obj_add_flag(usb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(usb, usb_drive_row_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *hdr = lv_obj_create(usb);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_width(hdr, lv_pct(100));
        lv_obj_set_height(hdr, 28);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(hdr);
        lv_label_set_text(lbl, "Export via USB");
        ui_theme_apply_label(lbl, false);
        lv_obj_set_style_text_font(lbl, ui_font_caption(), 0);
        lv_obj_set_flex_grow(lbl, 1);

        s_usb_drive_sw = lv_switch_create(hdr);
        lv_obj_clear_flag(s_usb_drive_sw, LV_OBJ_FLAG_CLICKABLE);
        ui_theme_apply_switch(s_usb_drive_sw);

        s_usb_status_lbl = lv_label_create(usb);
        ui_theme_apply_label(s_usb_status_lbl, true);
        lv_obj_set_style_text_font(s_usb_status_lbl, ui_font_caption(), 0);
        lv_label_set_text(s_usb_status_lbl, "");
        settings_page_sync_usb_state();
        update_sensors_label();
    }

    {
        lv_obj_t *device = section_group(s_body, "Device");
        s_datetime_lbl = create_value_row(device, "Date", "", false);
        update_datetime_label_text();
        if (s_datetime_timer) {
            lv_timer_del(s_datetime_timer);
            s_datetime_timer = NULL;
        }
        s_datetime_timer = lv_timer_create(datetime_timer_cb, 60000, NULL);
    }
}

void ui_settings_register_split_length_cb(ui_split_length_cb_t cb) { s_split_cb = cb; }

void settings_page_apply_theme(void)
{
    if (s_root)
        ui_status_bar_apply_theme(&s_status);
    if (s_split_val_lbl)
        ui_theme_apply_label(s_split_val_lbl, true);
    if (s_sensors_val_lbl)
        ui_theme_apply_label(s_sensors_val_lbl, true);
    if (s_datetime_lbl)
        ui_theme_apply_label(s_datetime_lbl, true);
    settings_page_set_dark_mode_state(ui_get_dark_mode());
    settings_page_sync_usb_state();
    settings_page_sync_sensors_state();
}

void settings_page_set_gps_status(bool connected, uint8_t bars)
{
    ui_status_bar_set_gps_status(&s_status, connected, bars);
}

void settings_page_set_dark_mode_state(bool enabled)
{
    if (!s_dark_mode_sw)
        return;
    if (enabled)
        lv_obj_add_state(s_dark_mode_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(s_dark_mode_sw, LV_STATE_CHECKED);
}

void settings_page_on_orientation_changed(void)
{
    ui_status_bar_force_refresh(&s_status);
}

void settings_page_open_split_dialog(void)
{
    create_split_dialog();
}

void settings_page_sync_usb_state(void)
{
    if (!s_usb_drive_sw)
        return;
    bool active = app_usb_msc_is_active();
    usb_switch_show(active);
    if (active)
    {
        usb_status_set("Drive ready - connect USB cable");
        return;
    }

    esp_err_t err = app_usb_msc_last_error();
    if (err != ESP_OK)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "USB failed: %s", esp_err_to_name(err));
        usb_status_set(buf);
        return;
    }

    if (!s_sd.mounted)
        usb_status_set("No SD card mounted");
    else
        usb_status_set("");
}

void settings_page_sync_sensors_state(void)
{
    update_sensors_label();
}
