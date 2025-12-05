#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_types.h"   // for i2c_port_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    uint8_t  pressure;
    bool     pressed;
} cst328_point_t;

// Initialise I2C bus (new driver) + CST328 device
esp_err_t cst328_init(i2c_port_t port,
                      gpio_num_t sda,
                      gpio_num_t scl,
                      gpio_num_t rst,
                      gpio_num_t irq,
                      uint32_t i2c_clk_hz);

// Poll one touch point
esp_err_t cst328_read_point(cst328_point_t *out_pt);

#ifdef __cplusplus
}
#endif
