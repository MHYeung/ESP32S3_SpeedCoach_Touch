#pragma once

#include <stdbool.h>
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