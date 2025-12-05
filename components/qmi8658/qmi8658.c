#include "qmi8658.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "qmi8658";

// Registers
#define REG_WHO_AM_I 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL7 0x08

#define REG_AX_L 0x35
#define REG_AX_H 0x36
#define REG_AY_L 0x37
#define REG_AY_H 0x38
#define REG_AZ_L 0x39
#define REG_AZ_H 0x3A

// WHO_AM_I value
#define QMI8658_WHO_AM_I_VAL 0x05

// Bits / fields we care about (accel only)
static esp_err_t qmi8658_write8(qmi8658_handle_t *imu, uint8_t reg, uint8_t val)
{
    return i2c_helper_write_reg(imu->dev, reg, &val, 1);
}

static esp_err_t qmi8658_read8(qmi8658_handle_t *imu, uint8_t reg, uint8_t *val)
{
    return i2c_helper_read_reg(imu->dev, reg, val, 1);
}

esp_err_t qmi8658_init(qmi8658_handle_t *imu,
                       i2c_helper_t *bus,
                       uint8_t addr_7bit)
{
    if (!imu || !bus)
        return ESP_ERR_INVALID_ARG;
    memset(imu, 0, sizeof(*imu));

    ESP_ERROR_CHECK(i2c_helper_add_device(bus, addr_7bit, &imu->dev));

    // WHO_AM_I
    uint8_t who = 0;
    ESP_ERROR_CHECK(qmi8658_read8(imu, REG_WHO_AM_I, &who));
    ESP_LOGI(TAG, "WHO_AM_I: 0x%02X", who);
    if (who != QMI8658_WHO_AM_I_VAL)
    {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I, expected 0x%02X", QMI8658_WHO_AM_I_VAL);
        return ESP_FAIL;
    }

    // CTRL1: enable auto-increment + internal osc, I2C mode
    //  - SPI_AI = 1 (auto increment)
    //  - rest mostly default; simplest is 0x60 (from typical configs)
    //    bit6=1 (auto increment), rest 0 for now
    ESP_ERROR_CHECK(qmi8658_write8(imu, REG_CTRL1, 0x60));

    // CTRL2: accelerometer range and ODR
    //  [6:4] aFS: 001=±4g
    //  [3:0] aODR: 0100 = 500 Hz (from datasheet table)
    //  => aFS=001 (4g) << 4 → 0x10, aODR=0x4 → 0x04, combined = 0x14
    uint8_t ctrl2 = 0x14;
    ESP_ERROR_CHECK(qmi8658_write8(imu, REG_CTRL2, ctrl2));

    // CTRL7: enable accelerometer (bit0 = aEN)
    //  aEN=1, gEN=0, mEN=0 -> 0x01
    ESP_ERROR_CHECK(qmi8658_write8(imu, REG_CTRL7, 0x01));

    // Scale: ±4 g over signed 16-bit → 4 / 32768
    float range_g = 4.0f;
    float g_to_ms2 = 9.80665f;
    imu->accel_scale = (range_g * g_to_ms2) / 32768.0f;

    ESP_LOGI(TAG, "QMI8658 init OK, addr=0x%02X, accel ±4g", addr_7bit);
    return ESP_OK;
}

esp_err_t qmi8658_read_accel(qmi8658_handle_t *imu,
                             float *ax_g,
                             float *ay_g,
                             float *az_g)
{
    if (!imu)
        return ESP_ERR_INVALID_ARG;

    uint8_t buf[6];
    esp_err_t err = i2c_helper_read_reg(imu->dev, REG_AX_L, buf, sizeof(buf));
    if (err != ESP_OK)
        return err;

    int16_t raw_x = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t raw_y = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t raw_z = (int16_t)((buf[5] << 8) | buf[4]);

    if (ax_g)
        *ax_g = raw_x * imu->accel_scale;
    if (ay_g)
        *ay_g = raw_y * imu->accel_scale;
    if (az_g)
        *az_g = raw_z * imu->accel_scale;

    return ESP_OK;
}
