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

void ui_set_orientation(ui_orientation_t o);

/**
 * Update IMU UI elements with new accelerometer data in SI units.
 *
 * ax, ay, az are in m/s^2.
 * This function is thread-safe and may be called from a FreeRTOS task.
 */
void ui_update_imu(float ax, float ay, float az);
