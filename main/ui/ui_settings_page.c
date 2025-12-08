// main/ui/ui_settings_page.c
#include "ui_settings_page.h"
#include "ui.h"       // for ui_notify_* functions
#include "ble.h"

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

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
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

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_txt);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *val = lv_label_create(row);
    if (value_txt) {
        lv_label_set_text(val, value_txt);
    } else {
        lv_label_set_text(val, "");
    }

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

/* -------------------------------------------------------------------------- */
/* BLE popup UI for manual connect                                            */
/* -------------------------------------------------------------------------- */

static lv_obj_t   *ble_dialog             = NULL;
static lv_obj_t   *ble_list               = NULL;
static lv_obj_t   *ble_status_label_popup = NULL;
static lv_timer_t *ble_list_timer         = NULL;

/* labels & widgets in main settings page */
static lv_obj_t *device_name_value_label   = NULL;
static lv_obj_t *ble_status_value_label    = NULL;
static lv_obj_t *ble_connection_row        = NULL;
static lv_obj_t *ble_connection_button     = NULL;
static lv_obj_t *ble_connection_name_label = NULL;

static bool s_ble_callbacks_registered = false;

/* remember last selected device name for display */
static char s_last_selected_name[BLE_NAME_MAX_LEN] = {0};

static void ble_dialog_refresh_list(void);
static void ble_dialog_device_clicked_cb(lv_event_t *e);
static void ble_dialog_close_cb(lv_event_t *e);
static void ble_list_timer_cb(lv_timer_t *t);
static void ble_connect_btn_event_cb(lv_event_t *e);

/* Refresh the device list inside the popup */
static void ble_dialog_refresh_list(void)
{
    if (!ble_dialog || !ble_list) {
        return;
    }

    lv_obj_clean(ble_list);

    int count = ble_get_device_count();
    char line[64];

    for (int i = 0; i < count; ++i) {
        ble_device_t dev;
        if (!ble_get_device(i, &dev)) {
            continue;
        }

        snprintf(line, sizeof(line), "%s (RSSI %d)",
                 dev.name[0] ? dev.name : "Unknown",
                 dev.rssi);

        lv_obj_t *btn = lv_list_add_btn(ble_list, NULL, line);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, ble_dialog_device_clicked_cb, LV_EVENT_CLICKED, NULL);
    }

    char status[32];
    snprintf(status, sizeof(status), "Found %d device(s)", count);
    lv_label_set_text(ble_status_label_popup, status);
}

/* User tapped a device in the list */
static void ble_dialog_device_clicked_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);  // LVGL 9.x API
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);

    // Save selected device name for later display
    ble_device_t dev;
    if (ble_get_device(idx, &dev)) {
        dev.name[BLE_NAME_MAX_LEN - 1] = '\0';
        strncpy(s_last_selected_name,
                dev.name[0] ? dev.name : "Unknown",
                BLE_NAME_MAX_LEN - 1);
        s_last_selected_name[BLE_NAME_MAX_LEN - 1] = '\0';
    } else {
        strncpy(s_last_selected_name, "Unknown", BLE_NAME_MAX_LEN);
    }

    lv_label_set_text(ble_status_label_popup, "Connecting...");

    // Stop scanning while connecting
    ble_stop_scan();
    (void)ble_connect_to_index(idx);

    // Close the dialog; main status will update via connection callback
    if (ble_list_timer) {
        lv_timer_del(ble_list_timer);
        ble_list_timer = NULL;
    }
    if (ble_dialog) {
        lv_obj_del(ble_dialog);
        ble_dialog = NULL;
    }
}

/* Close (X) button in popup */
static void ble_dialog_close_cb(lv_event_t *e)
{
    (void)e;

    ble_stop_scan();

    if (ble_list_timer) {
        lv_timer_del(ble_list_timer);
        ble_list_timer = NULL;
    }
    if (ble_dialog) {
        lv_obj_del(ble_dialog);
        ble_dialog = NULL;
    }
}

/* Periodic timer: refresh device list while scanning */
static void ble_list_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!ble_dialog) {
        if (ble_list_timer) {
            lv_timer_del(ble_list_timer);
            ble_list_timer = NULL;
        }
        return;
    }

    ble_dialog_refresh_list();
}

/* Create and show the BLE popup dialog */
static void ble_dialog_open(void)
{
    if (ble_dialog) {
        // Already open
        return;
    }

    // Start scanning
    ble_start_scan();

    // Create dialog on top layer so it stays above tabs (LVGL 9.x)
    ble_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ble_dialog, 230, 200);
    lv_obj_center(ble_dialog);

    // "Modal-like" behaviour for LVGL 9.x
    lv_obj_add_flag(ble_dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ble_dialog, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_set_style_bg_color(ble_dialog, lv_color_hex(0x006df6), 0);
    lv_obj_set_style_radius(ble_dialog, 10, 0);
    lv_obj_set_style_pad_all(ble_dialog, 10, 0);
    lv_obj_set_style_border_width(ble_dialog, 0, 0);

    // Title
    lv_obj_t *title = lv_label_create(ble_dialog);
    lv_label_set_text(title, "BLE: Select device");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    // Status label
    ble_status_label_popup = lv_label_create(ble_dialog);
    lv_label_set_text(ble_status_label_popup, "Scanning...");
    lv_obj_align(ble_status_label_popup, LV_ALIGN_TOP_LEFT, 0, 20);

    // Device list
    ble_list = lv_list_create(ble_dialog);
    lv_obj_set_size(ble_list, 210, 130);
    lv_obj_align(ble_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Close button
    lv_obj_t *close_btn = lv_btn_create(ble_dialog);
    lv_obj_set_size(close_btn, 24, 24);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, ble_dialog_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x_label = lv_label_create(close_btn);
    lv_label_set_text(x_label, LV_SYMBOL_CLOSE);
    lv_obj_center(x_label);

    // Timer to refresh list while scanning
    ble_list_timer = lv_timer_create(ble_list_timer_cb, 500, NULL);
}

/* Button handler on Settings row: open BLE dialog */
static void ble_connect_btn_event_cb(lv_event_t *e)
{
    (void)e;
    ble_dialog_open();
}

/* -------------------------------------------------------------------------- */
/* BLE connection-state callback -> updates status row & connection row       */
/* -------------------------------------------------------------------------- */

static void on_ble_connection_state(bool connected)
{
    // Update simple "BLE status" row
    if (ble_status_value_label) {
        lv_label_set_text(ble_status_value_label,
                          connected ? "Connected" : "Disconnected");
    }

    if (!ble_connection_row) {
        return;
    }

    if (connected) {
        // When connected: remove button and show "Connected: <name>"
        if (ble_connection_button) {
            lv_obj_del(ble_connection_button);
            ble_connection_button = NULL;
        }

        if (!ble_connection_name_label) {
            // Create value-label on the right of the row
            ble_connection_name_label = lv_label_create(ble_connection_row);
        }

        char buf[BLE_NAME_MAX_LEN + 20];
        const char *name = s_last_selected_name[0] ? s_last_selected_name : "Unknown";
        snprintf(buf, sizeof(buf), "Connected: %s", name);
        lv_label_set_text(ble_connection_name_label, buf);
    } else {
        // When disconnected: remove name label and show Connect button again
        if (ble_connection_name_label) {
            lv_obj_del(ble_connection_name_label);
            ble_connection_name_label = NULL;
        }

        if (!ble_connection_button) {
            ble_connection_button = lv_btn_create(ble_connection_row);
            lv_obj_set_size(ble_connection_button, 90, 30);
            lv_obj_add_event_cb(ble_connection_button,
                                ble_connect_btn_event_cb,
                                LV_EVENT_CLICKED, NULL);

            lv_obj_t *btn_lbl = lv_label_create(ble_connection_button);
            lv_label_set_text(btn_lbl, "Connect");
            lv_obj_center(btn_lbl);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Settings page entry point                                                  */
/* -------------------------------------------------------------------------- */

void settings_page_create(lv_obj_t *parent)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, lv_pct(100), lv_pct(100));
    lv_obj_center(cont);

    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    /* Dark mode row */
    create_settings_row(cont,
                        "Dark mode",
                        sw_dark_mode_event_cb,
                        false);

    /* Auto rotate row */
    create_settings_row(cont,
                        "Auto rotate",
                        sw_auto_rotate_event_cb,
                        true);

    /* Device name row (static for now, must match ble.c name) */
    device_name_value_label = create_value_row(cont,
                                               "Device",
                                               "ESP32S3-BLE");

    /* BLE connection status row */
    ble_status_value_label = create_value_row(cont,
                                              "BLE status",
                                              "Disconnected");

    /* BLE connection row:
       label on left, Connect button on right,
       which will be replaced by "Connected: <name>" when connected. */
    ble_connection_row = lv_obj_create(cont);
    lv_obj_set_width(ble_connection_row, lv_pct(100));
    lv_obj_set_height(ble_connection_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ble_connection_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ble_connection_row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ble_connection_row, 4, 0);
    lv_obj_set_style_border_width(ble_connection_row, 0, 0);

    lv_obj_t *ble_conn_lbl = lv_label_create(ble_connection_row);
    lv_label_set_text(ble_conn_lbl, "BLE connection");
    lv_obj_set_flex_grow(ble_conn_lbl, 1);

    // Initial Connect button
    ble_connection_button = lv_btn_create(ble_connection_row);
    lv_obj_set_size(ble_connection_button, 90, 30);
    lv_obj_add_event_cb(ble_connection_button,
                        ble_connect_btn_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(ble_connection_button);
    lv_label_set_text(btn_lbl, "Connect");
    lv_obj_center(btn_lbl);

    /* Register BLE callbacks once so status & connection row update
       when connect / disconnect events happen. */
    if (!s_ble_callbacks_registered) {
        ble_register_connection_state_callback(on_ble_connection_state);
        s_ble_callbacks_registered = true;
    }
}
