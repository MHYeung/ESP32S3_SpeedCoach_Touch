#pragma once

#include "esp_err.h"
#include "i2c_helper.h"

// Default address on your Waveshare board (per wiki) 
#define QMI8658_I2C_ADDR  0x6B

// Small struct to hold state
typedef struct {
    i2c_master_dev_handle_t dev;
    float accel_scale;   // m/s^2 per LSB
} qmi8658_handle_t;

// Initialize: WHO_AM_I check + accel config
esp_err_t qmi8658_init(qmi8658_handle_t *imu,
                       i2c_helper_t *bus,
                       uint8_t addr_7bit);

// Read accel in g
esp_err_t qmi8658_read_accel(qmi8658_handle_t *imu,
                             float *ax_g,
                             float *ay_g,
                             float *az_g);
