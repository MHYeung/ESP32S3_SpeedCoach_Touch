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

/**
 * Update IMU UI elements with new accelerometer data in SI units.
 *
 * ax, ay, az are in m/s^2.
 * This function is thread-safe and may be called from a FreeRTOS task.
 */
void ui_update_imu(float ax, float ay, float az);
