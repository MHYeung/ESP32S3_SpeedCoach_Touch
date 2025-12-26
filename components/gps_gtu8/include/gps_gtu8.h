#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;        // e.g. UART_NUM_1
    int tx_gpio;         // e.g. 43
    int rx_gpio;         // e.g. 44
    int baud;            // usually 9600
    int task_prio;       // e.g. 8
    int task_stack;      // e.g. 4096~6144
    int rx_buf_size;     // e.g. 2048
} gps_gtu8_config_t;

typedef struct {
    // Validity flags
    bool valid_fix;      // have a valid position fix
    bool valid_time;     // have hhmmss
    bool valid_date;     // have ddmmyy

    // GNSS data
    double lat_deg;      // decimal degrees, +N
    double lon_deg;      // decimal degrees, +E
    float  speed_mps;    // from RMC (knots -> m/s). NAN if not present
    float  course_deg;   // NAN if not present

    int    sats;         // from GGA, -1 if unknown
    float  hdop;         // from GGA, NAN if unknown
    int    fix_quality;  // 0 invalid, 1 GPS, 2 DGPS..., -1 if unknown

    // Time from GNSS (UTC)
    struct tm utc_tm;    // valid when valid_time && valid_date

    // Local timestamp when this fix was parsed
    int64_t rx_time_us;
} gps_fix_t;

typedef void (*gps_gtu8_cb_t)(const gps_fix_t *fix, void *user);

esp_err_t gps_gtu8_init(const gps_gtu8_config_t *cfg);
esp_err_t gps_gtu8_set_callback(gps_gtu8_cb_t cb, void *user);
bool      gps_gtu8_get_latest(gps_fix_t *out);

#ifdef __cplusplus
}
#endif
