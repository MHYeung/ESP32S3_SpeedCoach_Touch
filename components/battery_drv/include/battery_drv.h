#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct battery_drv_s *battery_drv_handle_t;

typedef struct {
    adc_unit_t unit;              // ADC_UNIT_1
    adc_channel_t channel;        // e.g. ADC_CHANNEL_7 (GPIO8 on ESP32-S3)
    adc_atten_t atten;            // ADC_ATTEN_DB_12
    adc_bitwidth_t bitwidth;      // ADC_BITWIDTH_DEFAULT

    // Scaling: Vbat = (Vadc_mv / 1000.0) * divider_ratio / measurement_offset
    float divider_ratio;          // example: 3.0f
    float measurement_offset;     // example: 0.9945f

    // Battery % mapping
    float v_empty;                // example: 3.30f
    float v_full;                 // example: 4.20f

    // Sampling
    uint8_t samples;              // e.g. 8 or 16 (average)
} battery_drv_config_t;

esp_err_t battery_drv_init(const battery_drv_config_t *cfg, battery_drv_handle_t *out);
void      battery_drv_deinit(battery_drv_handle_t h);

// Returns raw ADC voltage (after calibration if available) in mV
esp_err_t battery_drv_read_adc_mv(battery_drv_handle_t h, int *out_mv);

// Returns computed battery voltage in volts (V)
esp_err_t battery_drv_read_battery_v(battery_drv_handle_t h, float *out_v);

// Returns 0..100 (clamped) based on v_empty..v_full
esp_err_t battery_drv_read_percent(battery_drv_handle_t h, int *out_percent);

#ifdef __cplusplus
}
#endif
