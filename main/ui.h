#pragma once

#include "lvgl.h"

/**
 * Initialize the LVGL UI:
 * - Creates a tabview with:
 *   - Tab 1: Button test
 *   - Tab 2: IMU label + 3-axis chart
 *
 * Must be called after:
 *   - lvgl_port_init()
 *   - lvgl_port_add_disp() (so we have a valid lv_disp_t*)
 */
void ui_init(lv_disp_t *disp);

typedef enum {
    UI_ORIENT_PORTRAIT_0,
    UI_ORIENT_LANDSCAPE_90,
    UI_ORIENT_PORTRAIT_180,
    UI_ORIENT_LANDSCAPE_270,
} ui_orientation_t;

typedef enum {
    UI_PAGE_CONTROLS = 0,
    UI_PAGE_IMU,
    UI_PAGE_SYSTEM,
    UI_PAGE_SETTINGS,
    UI_PAGE_COUNT,          // keep this as last
} ui_page_t;

void ui_set_orientation(ui_orientation_t o);

/**
 * Update IMU UI elements with new accelerometer data in SI units.
 *
 * ax, ay, az are in m/s^2.
 * This function is thread-safe and may be called from a FreeRTOS task.
 */
void ui_update_imu(float ax, float ay, float az);

/* Navigate to a page by logical name */
void ui_go_to_page(ui_page_t page, bool animated);

typedef void (*ui_sd_test_cb_t)(void);
void ui_register_sd_test_cb(ui_sd_test_cb_t cb);
