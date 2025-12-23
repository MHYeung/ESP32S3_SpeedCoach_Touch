#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "i2c_helper.h"

// PCF85063 7-bit I2C address
#define PCF85063_ADDRESS   (0x51)

// Waveshare uses YEAR_OFFSET=1970 (it stores year as 00..99)
#define YEAR_OFFSET        (1970)

// Register map
#define RTC_CTRL_1_ADDR     (0x00)
#define RTC_CTRL_2_ADDR     (0x01)
#define RTC_OFFSET_ADDR     (0x02)
#define RTC_RAM_by_ADDR     (0x03)

#define RTC_SECOND_ADDR     (0x04)
#define RTC_MINUTE_ADDR     (0x05)
#define RTC_HOUR_ADDR       (0x06)
#define RTC_DAY_ADDR        (0x07)
#define RTC_WEEKDAY_ADDR    (0x08)
#define RTC_MONTH_ADDR      (0x09)
#define RTC_YEAR_ADDR       (0x0A)

// Alarm regs
#define RTC_SECOND_ALARM    (0x0B)
#define RTC_MINUTE_ALARM    (0x0C)
#define RTC_HOUR_ALARM      (0x0D)
#define RTC_DAY_ALARM       (0x0E)
#define RTC_WDAY_ALARM      (0x0F)

// CTRL2 bits (common usage in Waveshare code)
#define RTC_CTRL_2_AIE      (0x02)
#define RTC_CTRL_2_AF       (0x08)

// CTRL1 CAP_SEL bit used by Waveshare (load capacitance 12.5pF)
#define RTC_CTRL_1_CAP_SEL  (0x01)

// Seconds register OS bit (Oscillator Stop flag)
#define RTC_SECOND_OSF      (0x80)

// format helpers
#define RTC_ALARM           (0x80)  // AEN_x bit: 1=disabled compare for that field

typedef struct {
    uint16_t year;   // e.g. 2025
    uint8_t  month;  // 1-12
    uint8_t  day;    // 1-31
    uint8_t  dotw;   // 0-6  (0=Sunday .. 6=Saturday)
    uint8_t  hour;   // 0-23
    uint8_t  minute; // 0-59
    uint8_t  second; // 0-59
} datetime_t;

// Optional global (Waveshare style)
extern datetime_t datetime;

/**
 * @brief Attach PCF85063 to an existing i2c_helper bus and init control regs.
 *
 * You should call i2c_helper_init() once in your app, then call this.
 */
esp_err_t PCF85063_init(i2c_helper_t *i2c);

/** @brief Read only time+date (seconds..year) */
esp_err_t PCF85063_read_time(datetime_t *time);

/** @brief Set only time (sec/min/hour) */
esp_err_t PCF85063_set_time(datetime_t time);

/** @brief Set only date (day/dotw/month/year) */
esp_err_t PCF85063_set_date(datetime_t date);

/** @brief Set time+date (sec..year) */
esp_err_t PCF85063_set_all(datetime_t time);

/** @brief Enable alarm interrupt (AIE=1) and clear AF */
esp_err_t PCF85063_enable_alarm(void);

/** @brief Read CTRL2 and return (AF|AIE) masked */
esp_err_t PCF85063_get_alarm_flag(uint8_t *out_masked);

/** @brief Set alarm (sec/min/hour) and disable day/weekday compares */
esp_err_t PCF85063_set_alarm(datetime_t time);

/** @brief Read alarm regs */
esp_err_t PCF85063_read_alarm(datetime_t *time);

/** @brief Convert datetime to "YYYY-MM-DD HH:MM:SS" string */
void datetime_to_str(char *datetime_str, datetime_t time);

/** @brief Check if oscillator stop flag is clear (time valid) */
esp_err_t PCF85063_is_time_valid(bool *valid);

/** @brief Clear oscillator stop flag (OSF) */
esp_err_t PCF85063_clear_OSF(void);

/** @brief Convenience: seconds since midnight (for your status bar base) */
uint32_t PCF85063_seconds_since_midnight(const datetime_t *t);
