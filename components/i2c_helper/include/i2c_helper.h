#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

typedef struct
{
    i2c_master_bus_handle_t bus;
    uint32_t clk_hz;
} i2c_helper_t;

// Initialize an I2C master bus (one per port)
esp_err_t i2c_helper_init(i2c_helper_t *ctx,
                          int port,
                          int sda_gpio,
                          int scl_gpio,
                          uint32_t clk_hz);

// Add a device (e.g. QMI8658) to the bus
esp_err_t i2c_helper_add_device(i2c_helper_t *ctx,
                                uint8_t addr_7bit,
                                i2c_master_dev_handle_t *out_dev);

// Simple “register style” helpers
esp_err_t i2c_helper_write_reg(i2c_master_dev_handle_t dev,
                               uint8_t reg,
                               const uint8_t *data,
                               size_t len);

esp_err_t i2c_helper_read_reg(i2c_master_dev_handle_t dev,
                              uint8_t reg,
                              uint8_t *data,
                              size_t len);