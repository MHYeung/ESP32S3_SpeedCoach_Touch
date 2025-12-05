#include "i2c_helper.h"

static const char *TAG = "i2c_helper";

esp_err_t i2c_helper_init(i2c_helper_t *ctx,
                          int port,
                          int sda_gpio,
                          int scl_gpio,
                          uint32_t clk_hz)
{
    if (!ctx)
        return ESP_ERR_INVALID_ARG;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .scl_io_num = scl_gpio,
        .sda_io_num = sda_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &ctx->bus);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->clk_hz = clk_hz; // remember the desired speed

    ESP_LOGI(TAG, "I2C bus init OK: port=%d SDA=%d SCL=%d clk=%lu",
             port, sda_gpio, scl_gpio, (unsigned long)clk_hz);
    return ESP_OK;
}

esp_err_t i2c_helper_add_device(i2c_helper_t *ctx,
                                uint8_t addr_7bit,
                                i2c_master_dev_handle_t *out_dev)
{
    if (!ctx || !out_dev) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr_7bit,
        .scl_speed_hz    = ctx->clk_hz,   // <-- MUST be non-zero and valid
    };

    esp_err_t err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device addr=0x%02X failed: %s",
                 addr_7bit, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2C device added: addr=0x%02X, clk=%lu",
             addr_7bit, (unsigned long)ctx->clk_hz);
    return ESP_OK;
}

esp_err_t i2c_helper_write_reg(i2c_master_dev_handle_t dev,
                               uint8_t reg,
                               const uint8_t *data,
                               size_t len)
{
    uint8_t buf[1 + len];
    buf[0] = reg;
    if (len > 0 && data)
    {
        memcpy(&buf[1], data, len);
    }

    return i2c_master_transmit(dev, buf, 1 + len, -1);
}

esp_err_t i2c_helper_read_reg(i2c_master_dev_handle_t dev,
                              uint8_t reg,
                              uint8_t *data,
                              size_t len)
{
    // write register address, then read
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, -1);
}
