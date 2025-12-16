#pragma once

#include "esp_err.h"
#include "i2c_helper.h"

// Default address on your Waveshare board (per wiki) 
#define QMI8658_I2C_ADDR  0x6B

// Small struct to hold state
typedef struct {
    i2c_master_dev_handle_t dev;
    float accel_scale;   // m/s^2 per LSB
    float gyro_scale;    // rad/s per LSB
} qmi8658_handle_t;

// Initialize: WHO_AM_I check + accel config
esp_err_t qmi8658_init(qmi8658_handle_t *imu,
                       i2c_helper_t *bus,
                       uint8_t addr_7bit);

/* Accel in m/s^2 */
esp_err_t qmi8658_read_accel(qmi8658_handle_t *imu,
                             float *ax_mps2,
                             float *ay_mps2,
                             float *az_mps2);

/* Gyro in rad/s */
esp_err_t qmi8658_read_gyro(qmi8658_handle_t *imu,
                            float *gx_rads,
                            float *gy_rads,
                            float *gz_rads);

/* Single burst read for best timing */
esp_err_t qmi8658_read_accel_gyro(qmi8658_handle_t *imu,
                                 float *ax_mps2, float *ay_mps2, float *az_mps2,
                                 float *gx_rads, float *gy_rads, float *gz_rads);