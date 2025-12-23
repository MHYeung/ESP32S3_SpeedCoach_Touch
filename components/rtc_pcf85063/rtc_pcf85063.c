#include "rtc_pcf85063.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

datetime_t datetime = {0};

static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_lock;

static uint8_t decToBcd(int val)
{
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

static uint8_t bcdToDec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static esp_err_t rtc_write(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_helper_write_reg(s_dev, reg, data, len);
}

static esp_err_t rtc_read(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_helper_read_reg(s_dev, reg, data, len);
}

esp_err_t PCF85063_init(i2c_helper_t *i2c)
{
    if (!i2c) return ESP_ERR_INVALID_ARG;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    // Attach device on the shared bus
    esp_err_t err = i2c_helper_add_device(i2c, PCF85063_ADDRESS, &s_dev);
    if (err != ESP_OK) return err;

    // Waveshare default: CTRL1 = DEFAULT | CAP_SEL
    uint8_t ctrl1 = (uint8_t)(RTC_CTRL_1_CAP_SEL); // DEFAULT(0) | CAP_SEL
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = rtc_write(RTC_CTRL_1_ADDR, &ctrl1, 1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    // CTRL2 default = 0
    uint8_t ctrl2 = 0x00;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = rtc_write(RTC_CTRL_2_ADDR, &ctrl2, 1);
    xSemaphoreGive(s_lock);

    return err;
}

esp_err_t PCF85063_read_time(datetime_t *time)
{
    if (!time) return ESP_ERR_INVALID_ARG;

    uint8_t buf[7] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_SECOND_ADDR, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    time->second = bcdToDec(buf[0] & 0x7F);
    time->minute = bcdToDec(buf[1] & 0x7F);
    time->hour   = bcdToDec(buf[2] & 0x3F);
    time->day    = bcdToDec(buf[3] & 0x3F);
    time->dotw   = bcdToDec(buf[4] & 0x07);
    time->month  = bcdToDec(buf[5] & 0x1F);
    time->year   = (uint16_t)(bcdToDec(buf[6]) + YEAR_OFFSET);

    return ESP_OK;
}

esp_err_t PCF85063_set_time(datetime_t time)
{
    uint8_t buf[3] = {
        decToBcd(time.second),
        decToBcd(time.minute),
        decToBcd(time.hour),
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_write(RTC_SECOND_ADDR, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t PCF85063_set_date(datetime_t date)
{
    uint8_t buf[4] = {
        decToBcd(date.day),
        decToBcd(date.dotw),
        decToBcd(date.month),
        decToBcd((int)(date.year - YEAR_OFFSET)),
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_write(RTC_DAY_ADDR, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t PCF85063_set_all(datetime_t time)
{
    uint8_t buf[7] = {
        (uint8_t)(decToBcd(time.second) & 0x7F), // ensure OSF bit cleared on write
        (uint8_t)(decToBcd(time.minute) & 0x7F),
        (uint8_t)(decToBcd(time.hour)   & 0x3F),
        (uint8_t)(decToBcd(time.day)    & 0x3F),
        (uint8_t)(decToBcd(time.dotw)   & 0x07),
        (uint8_t)(decToBcd(time.month)  & 0x1F),
        decToBcd((int)(time.year - YEAR_OFFSET)),
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_write(RTC_SECOND_ADDR, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    // Clear OSF explicitly (safe)
    return PCF85063_clear_OSF();
}

esp_err_t PCF85063_enable_alarm(void)
{
    // CTRL2: set AIE, clear AF
    uint8_t val = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_CTRL_2_ADDR, &val, 1);
    if (err == ESP_OK) {
        val |= RTC_CTRL_2_AIE;
        val &= (uint8_t)~RTC_CTRL_2_AF;
        err = rtc_write(RTC_CTRL_2_ADDR, &val, 1);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t PCF85063_get_alarm_flag(uint8_t *out_masked)
{
    if (!out_masked) return ESP_ERR_INVALID_ARG;

    uint8_t val = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_CTRL_2_ADDR, &val, 1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    *out_masked = (uint8_t)(val & (RTC_CTRL_2_AF | RTC_CTRL_2_AIE));
    return ESP_OK;
}

/**
 * Fix from Waveshare bug:
 * - Their code declared buf[5] but wrote 6 bytes.
 * - Correct alarm payload is 5 bytes: sec, min, hour, day, weekday
 */
esp_err_t PCF85063_set_alarm(datetime_t time)
{
    uint8_t buf[5] = {
        (uint8_t)(decToBcd(time.second) & (uint8_t)~RTC_ALARM),
        (uint8_t)(decToBcd(time.minute) & (uint8_t)~RTC_ALARM),
        (uint8_t)(decToBcd(time.hour)   & (uint8_t)~RTC_ALARM),
        RTC_ALARM, // disable day compare
        RTC_ALARM  // disable weekday compare
    };

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_write(RTC_SECOND_ALARM, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t PCF85063_read_alarm(datetime_t *time)
{
    if (!time) return ESP_ERR_INVALID_ARG;

    uint8_t buf[5] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_SECOND_ALARM, buf, sizeof(buf));
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    time->second = bcdToDec(buf[0] & 0x7F);
    time->minute = bcdToDec(buf[1] & 0x7F);
    time->hour   = bcdToDec(buf[2] & 0x3F);
    time->day    = bcdToDec(buf[3] & 0x3F);
    time->dotw   = bcdToDec(buf[4] & 0x07);

    return ESP_OK;
}

void datetime_to_str(char *datetime_str, datetime_t time)
{
    if (!datetime_str) return;
    // "YYYY-MM-DD HH:MM:SS" => 19 chars + null
    snprintf(datetime_str, 32, "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)time.year,
             (unsigned)time.month,
             (unsigned)time.day,
             (unsigned)time.hour,
             (unsigned)time.minute,
             (unsigned)time.second);
}

esp_err_t PCF85063_is_time_valid(bool *valid)
{
    if (!valid) return ESP_ERR_INVALID_ARG;

    uint8_t sec = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_SECOND_ADDR, &sec, 1);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) return err;

    *valid = ((sec & RTC_SECOND_OSF) == 0);
    return ESP_OK;
}

esp_err_t PCF85063_clear_OSF(void)
{
    uint8_t sec = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = rtc_read(RTC_SECOND_ADDR, &sec, 1);
    if (err == ESP_OK) {
        sec &= (uint8_t)~RTC_SECOND_OSF;
        err = rtc_write(RTC_SECOND_ADDR, &sec, 1);
    }
    xSemaphoreGive(s_lock);
    return err;
}

uint32_t PCF85063_seconds_since_midnight(const datetime_t *t)
{
    if (!t) return 0;
    return (uint32_t)t->hour * 3600u + (uint32_t)t->minute * 60u + (uint32_t)t->second;
}
