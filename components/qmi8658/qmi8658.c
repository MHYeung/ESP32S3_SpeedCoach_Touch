#include "qmi8658.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <math.h>

static const char *TAG = "qmi8658";

// Registers
#define REG_WHO_AM_I 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03
#define REG_CTRL3 0x04
#define REG_CTRL5 0x06
#define REG_CTRL7 0x08

#define REG_AX_L 0x35
#define REG_GX_L 0x3B

// WHO_AM_I value
#define QMI8658_WHO_AM_I_VAL 0x05

// CTRL2 (Accel): aFS[6:4], aODR[3:0]
#define QMI8658_AFS_8G 0x02     // 010b
#define QMI8658_AODR_235HZ 0x05 // 0101b (6DOF effective ~235Hz)

// CTRL3 (Gyro): gFS[6:4], gODR[3:0]
#define QMI8658_GFS_512DPS 0x05 // 101b (closest to 500 dps)
#define QMI8658_GODR_235HZ 0x05 // 0101b

// CTRL5 LPF: enable accel+gyro LPF, BW ~5.39% ODR (mode=10) for both
#define QMI8658_CTRL5_LPF_BW_5P39_ODR 0x55

// CTRL1: ADDR_AI=1, BE=1 (matches your original 0x60)
#define QMI8658_CTRL1_DEFAULT 0x60

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

    ESP_RETURN_ON_ERROR(i2c_helper_add_device(bus, addr_7bit, &imu->dev), TAG, "");

    // WHO_AM_I
    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(qmi8658_read8(imu, REG_WHO_AM_I, &who), TAG, "");
    ESP_LOGI(TAG, "WHO_AM_I: 0x%02X", who);
    if (who != QMI8658_WHO_AM_I_VAL)
    {
        ESP_LOGE(TAG, "Unexpected WHO_AM_I, expected 0x%02X", QMI8658_WHO_AM_I_VAL);
        return ESP_FAIL;
    }

    // CTRL1: enable auto-increment, big-endian (per your current setup)
    ESP_RETURN_ON_ERROR(qmi8658_write8(imu, REG_CTRL1, QMI8658_CTRL1_DEFAULT), TAG, "");

    // CTRL5: enable LPF for both accel+gyro (recommended for stroke detection)
    ESP_RETURN_ON_ERROR(qmi8658_write8(imu, REG_CTRL5, QMI8658_CTRL5_LPF_BW_5P39_ODR), TAG, "");

    // CTRL2: accel ±8g, ODR setting code 0x5 (effective ~235Hz in 6DOF mode)
    uint8_t ctrl2 = (uint8_t)((QMI8658_AFS_8G << 4) | (QMI8658_AODR_235HZ & 0x0F));
    ESP_RETURN_ON_ERROR(qmi8658_write8(imu, REG_CTRL2, ctrl2), TAG, "");

    // CTRL3: gyro ±512 dps, ODR 235Hz
    uint8_t ctrl3 = (uint8_t)((QMI8658_GFS_512DPS << 4) | (QMI8658_GODR_235HZ & 0x0F));
    ESP_RETURN_ON_ERROR(qmi8658_write8(imu, REG_CTRL3, ctrl3), TAG, "");

    // CTRL7: enable accel + gyro (aEN=bit0, gEN=bit1)
    ESP_RETURN_ON_ERROR(qmi8658_write8(imu, REG_CTRL7, 0x03), TAG, "");

    // Scales (16-bit signed full scale)
    imu->accel_scale = (8.0f * 9.80665f) / 32768.0f;                // m/s^2 per LSB
    imu->gyro_scale = ((512.0f / 32768.0f) * (float)M_PI) / 180.0f; // rad/s per LSB

    ESP_LOGI(TAG, "QMI8658 init OK addr=0x%02X accel=±8g gyro=±512dps odr~235Hz", addr_7bit);
    return ESP_OK;
}

esp_err_t qmi8658_read_accel(qmi8658_handle_t *imu,
                             float *ax_mps2,
                             float *ay_mps2,
                             float *az_mps2)
{
    return qmi8658_read_accel_gyro(imu, ax_mps2, ay_mps2, az_mps2, NULL, NULL, NULL);
}

esp_err_t qmi8658_read_gyro(qmi8658_handle_t *imu,
                            float *gx_rads,
                            float *gy_rads,
                            float *gz_rads)
{
    return qmi8658_read_accel_gyro(imu, NULL, NULL, NULL, gx_rads, gy_rads, gz_rads);
}

esp_err_t qmi8658_read_accel_gyro(qmi8658_handle_t *imu,
                                  float *ax_mps2, float *ay_mps2, float *az_mps2,
                                  float *gx_rads, float *gy_rads, float *gz_rads)
{
    if (!imu)
        return ESP_ERR_INVALID_ARG;

    uint8_t buf[12];
    esp_err_t err = i2c_helper_read_reg(imu->dev, REG_AX_L, buf, sizeof(buf));
    if (err != ESP_OK)
        return err;

    int16_t raw_ax = (int16_t)((buf[1] << 8) | buf[0]);
    int16_t raw_ay = (int16_t)((buf[3] << 8) | buf[2]);
    int16_t raw_az = (int16_t)((buf[5] << 8) | buf[4]);
    int16_t raw_gx = (int16_t)((buf[7] << 8) | buf[6]);
    int16_t raw_gy = (int16_t)((buf[9] << 8) | buf[8]);
    int16_t raw_gz = (int16_t)((buf[11] << 8) | buf[10]);

    if (ax_mps2)
        *ax_mps2 = raw_ax * imu->accel_scale;
    if (ay_mps2)
        *ay_mps2 = raw_ay * imu->accel_scale;
    if (az_mps2)
        *az_mps2 = raw_az * imu->accel_scale;

    if (gx_rads)
        *gx_rads = raw_gx * imu->gyro_scale;
    if (gy_rads)
        *gy_rads = raw_gy * imu->gyro_scale;
    if (gz_rads)
        *gz_rads = raw_gz * imu->gyro_scale;

    return ESP_OK;
}
