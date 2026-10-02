#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Initialize NVS. Handles first-time setup and error recovery.
 * Replaces your local app_nvs_init().
 */
esp_err_t nvs_helper_init(void);

/**
 * Activity ID: Reads the last ID, increments it, saves it, and returns the new one.
 * Returns 1 if no previous ID is found.
 */
uint32_t nvs_helper_get_next_activity_id(void);

/* Settings: Getters (Load on boot) */
bool nvs_helper_get_dark_mode(void);        // Default: false
bool nvs_helper_get_auto_rotate(void);      // Default: true
uint32_t nvs_helper_get_split_len(void);    // Default: 1000m
uint8_t nvs_helper_get_orientation(void);   // Returns default (e.g., 0)

/* Settings: Setters (Save when changed) */
void nvs_helper_set_dark_mode(bool enabled);
void nvs_helper_set_auto_rotate(bool enabled);
void nvs_helper_set_split_len(uint32_t len_m);
void nvs_helper_set_orientation(uint8_t orient);

/* Live-screen metric slots (0..2). Values are data_metric_t stored as u8. */
void nvs_helper_get_data_metrics(uint8_t out[3]);
void nvs_helper_set_data_metrics(const uint8_t in[3]);
bool nvs_helper_get_metrics_lock(void);     /* Default: true (lock while recording) */
void nvs_helper_set_metrics_lock(bool enabled);

uint8_t nvs_helper_get_brightness(void);    /* Default: 80 percent */
void nvs_helper_set_brightness(uint8_t percent);
bool nvs_helper_get_auto_dim(void);         /* Default: true */
void nvs_helper_set_auto_dim(bool enabled);

bool nvs_helper_get_sensors_enabled(void);  /* Default: false */
void nvs_helper_set_sensors_enabled(bool enabled);
void nvs_helper_get_sensor_addr(uint8_t out[6]);
void nvs_helper_set_sensor_addr(const uint8_t in[6]);
uint8_t nvs_helper_get_sensor_addr_type(void);
void nvs_helper_set_sensor_addr_type(uint8_t addr_type);
void nvs_helper_get_sensor_name(char *out, size_t out_len);
void nvs_helper_set_sensor_name(const char *name);

/* Heart-rate strap. Separate from sn_addr so an existing RowPod pairing survives. */
void nvs_helper_get_hr_addr(uint8_t out[6]);
void nvs_helper_set_hr_addr(const uint8_t in[6]);
uint8_t nvs_helper_get_hr_addr_type(void);
void nvs_helper_set_hr_addr_type(uint8_t addr_type);
void nvs_helper_get_hr_name(char *out, size_t out_len);
void nvs_helper_set_hr_name(const char *name);