#include "gps_gtu8.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gps_gtu8";

static gps_fix_t s_latest;
static SemaphoreHandle_t s_lock;

static gps_gtu8_cb_t s_cb;
static void *s_cb_user;

static int s_uart = -1;

static bool nmea_checksum_ok(const char *line)
{
    // Accept lines without checksum too (some modules do weird things)
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star) return true;

    uint8_t cs = 0;
    for (const char *p = line + 1; p < star; p++) cs ^= (uint8_t)(*p);

    char *end = NULL;
    long got = strtol(star + 1, &end, 16);
    if (end == (star + 1)) return false;
    return ((uint8_t)got) == cs;
}

static int split_fields(char *s, char *fields[], int max_fields)
{
    int n = 0;
    if (!s || !fields || max_fields <= 0) return 0;

    // strip leading $
    if (*s == '$') s++;

    fields[n++] = s;
    for (char *p = s; *p && n < max_fields; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        } else if (*p == '*') {
            *p = '\0';
            break;
        }
    }
    return n;
}

// ddmm.mmmm -> decimal degrees
static double dm_to_deg(const char *dm)
{
    if (!dm || !dm[0]) return NAN;
    double v = strtod(dm, NULL);
    int deg = (int)(v / 100.0);
    double minutes = v - (double)deg * 100.0;
    return (double)deg + minutes / 60.0;
}

static bool parse_hhmmss(const char *s, int *hh, int *mm, int *ss)
{
    if (!s || strlen(s) < 6) return false;
    char buf[3] = {0};

    buf[0] = s[0]; buf[1] = s[1]; *hh = atoi(buf);
    buf[0] = s[2]; buf[1] = s[3]; *mm = atoi(buf);
    buf[0] = s[4]; buf[1] = s[5]; *ss = atoi(buf);
    return true;
}

static bool parse_ddmmyy(const char *s, int *dd, int *mo, int *yy)
{
    if (!s || strlen(s) < 6) return false;
    char buf[3] = {0};

    buf[0] = s[0]; buf[1] = s[1]; *dd = atoi(buf);
    buf[0] = s[2]; buf[1] = s[3]; *mo = atoi(buf);
    buf[0] = s[4]; buf[1] = s[5]; *yy = atoi(buf);
    return true;
}

static void handle_rmc(char *fields[], int n, gps_fix_t *fix)
{
    // RMC: 0=GPRMC/GNRMC, 1=time, 2=status(A/V), 3=lat,4=N/S, 5=lon,6=E/W,
    // 7=speed(knots), 8=course, 9=date(ddmmyy)
    if (n < 10) return;

    // time
    int hh=0, mm=0, ss=0;
    if (parse_hhmmss(fields[1], &hh, &mm, &ss)) {
        fix->valid_time = true;
        fix->utc_tm.tm_hour = hh;
        fix->utc_tm.tm_min  = mm;
        fix->utc_tm.tm_sec  = ss;
    }

    // status
    bool active = (fields[2] && fields[2][0] == 'A');

    // lat/lon
    double lat = dm_to_deg(fields[3]);
    double lon = dm_to_deg(fields[5]);
    if (isfinite(lat) && isfinite(lon)) {
        if (fields[4] && fields[4][0] == 'S') lat = -lat;
        if (fields[6] && fields[6][0] == 'W') lon = -lon;
        fix->lat_deg = lat;
        fix->lon_deg = lon;
    }

    // speed knots -> m/s
    if (fields[7] && fields[7][0]) {
        double kn = strtod(fields[7], NULL);
        fix->speed_mps = (float)(kn * 0.514444);
    }

    // course
    if (fields[8] && fields[8][0]) {
        fix->course_deg = (float)strtod(fields[8], NULL);
    }

    // date ddmmyy
    int dd=0, mo=0, yy=0;
    if (parse_ddmmyy(fields[9], &dd, &mo, &yy)) {
        fix->valid_date = true;
        // yy is 00..99 (assume 2000+)
        int year = 2000 + yy;
        fix->utc_tm.tm_year = year - 1900;
        fix->utc_tm.tm_mon  = mo - 1;
        fix->utc_tm.tm_mday = dd;
    }

    // If active + lat/lon exists, treat as valid fix
    if (active && isfinite(fix->lat_deg) && isfinite(fix->lon_deg)) {
        fix->valid_fix = true;
    }
}

static void handle_gga(char *fields[], int n, gps_fix_t *fix)
{
    // GGA: 0=GPGGA/GNGGA, 6=fix quality, 7=sats, 8=hdop
    if (n < 9) return;

    if (fields[6] && fields[6][0]) fix->fix_quality = atoi(fields[6]);
    if (fields[7] && fields[7][0]) fix->sats = atoi(fields[7]);
    if (fields[8] && fields[8][0]) fix->hdop = (float)strtod(fields[8], NULL);
}

static void parse_line(const char *line_in)
{
    if (!line_in || line_in[0] != '$') return;
    if (!nmea_checksum_ok(line_in)) return;

    gps_fix_t upd = {0};
    upd.lat_deg = NAN;
    upd.lon_deg = NAN;
    upd.speed_mps = NAN;
    upd.course_deg = NAN;
    upd.hdop = NAN;
    upd.sats = -1;
    upd.fix_quality = -1;
    memset(&upd.utc_tm, 0, sizeof(upd.utc_tm));
    upd.rx_time_us = esp_timer_get_time();

    // Copy to mutable buffer
    char buf[128];
    size_t L = strnlen(line_in, sizeof(buf)-1);
    memcpy(buf, line_in, L);
    buf[L] = '\0';

    // Trim CRLF
    for (int i = (int)L - 1; i >= 0; i--) {
        if (buf[i] == '\r' || buf[i] == '\n') buf[i] = '\0';
        else break;
    }

    char *fields[24] = {0};
    int n = split_fields(buf, fields, 24);
    if (n <= 0) return;

    const char *type = fields[0];
    if (!type) return;

    if (strcmp(type, "GPRMC") == 0 || strcmp(type, "GNRMC") == 0) {
        handle_rmc(fields, n, &upd);
    } else if (strcmp(type, "GPGGA") == 0 || strcmp(type, "GNGGA") == 0) {
        handle_gga(fields, n, &upd);
    } else {
        return; // ignore others for now
    }

    // Merge into latest (RMC + GGA arrive separately)
    xSemaphoreTake(s_lock, portMAX_DELAY);

    // If this update has position, overwrite position
    if (isfinite(upd.lat_deg) && isfinite(upd.lon_deg)) {
        s_latest.lat_deg = upd.lat_deg;
        s_latest.lon_deg = upd.lon_deg;
    }
    // If has speed/course, overwrite
    if (isfinite(upd.speed_mps)) s_latest.speed_mps = upd.speed_mps;
    if (isfinite(upd.course_deg)) s_latest.course_deg = upd.course_deg;

    // If has fix meta, overwrite
    if (upd.sats >= 0) s_latest.sats = upd.sats;
    if (isfinite(upd.hdop)) s_latest.hdop = upd.hdop;
    if (upd.fix_quality >= 0) s_latest.fix_quality = upd.fix_quality;

    // Valid flags: accumulate
    s_latest.valid_fix  = s_latest.valid_fix  || upd.valid_fix;
    s_latest.valid_time = s_latest.valid_time || upd.valid_time;
    s_latest.valid_date = s_latest.valid_date || upd.valid_date;

    // If we got a full date+time, store it
    if (upd.valid_time) {
        s_latest.utc_tm.tm_hour = upd.utc_tm.tm_hour;
        s_latest.utc_tm.tm_min  = upd.utc_tm.tm_min;
        s_latest.utc_tm.tm_sec  = upd.utc_tm.tm_sec;
    }
    if (upd.valid_date) {
        s_latest.utc_tm.tm_year = upd.utc_tm.tm_year;
        s_latest.utc_tm.tm_mon  = upd.utc_tm.tm_mon;
        s_latest.utc_tm.tm_mday = upd.utc_tm.tm_mday;
    }

    s_latest.rx_time_us = upd.rx_time_us;

    gps_fix_t cb_copy = s_latest;
    gps_gtu8_cb_t cb = s_cb;
    void *cb_user = s_cb_user;

    static int s_printed = 0;
    if (s_printed < 10) {
        ESP_LOGI("gps_gtu8", "NMEA: %s", line_in);
        s_printed++;
    }

    xSemaphoreGive(s_lock);

    if (cb) cb(&cb_copy, cb_user);
}

static void gps_task(void *arg)
{
    (void)arg;

    uint8_t rx[256];
    char line[160];
    int line_len = 0;

    while (1) {
        int n = uart_read_bytes(s_uart, rx, sizeof(rx), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];
            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) parse_line(line);
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < (int)sizeof(line) - 1) {
                    line[line_len++] = c;
                } else {
                    // overflow: reset
                    line_len = 0;
                }
            }
        }
    }
}

esp_err_t gps_gtu8_init(const gps_gtu8_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    // init defaults
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_latest.lat_deg = NAN;
    s_latest.lon_deg = NAN;
    s_latest.speed_mps = NAN;
    s_latest.course_deg = NAN;
    s_latest.hdop = NAN;
    s_latest.sats = -1;
    s_latest.fix_quality = -1;
    xSemaphoreGive(s_lock);

    s_uart = cfg->uart_num;

    uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s_uart, &uc));
    ESP_ERROR_CHECK(uart_set_pin(s_uart, cfg->tx_gpio, cfg->rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_ERROR_CHECK(uart_driver_install(s_uart, cfg->rx_buf_size, 0, 0, NULL, 0));

    xTaskCreate(gps_task, "gps_gtu8", cfg->task_stack, NULL, cfg->task_prio, NULL);
    ESP_LOGI(TAG, "GPS init uart=%d tx=%d rx=%d baud=%d", cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, cfg->baud);

    return ESP_OK;
}

esp_err_t gps_gtu8_set_callback(gps_gtu8_cb_t cb, void *user)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cb = cb;
    s_cb_user = user;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool gps_gtu8_get_latest(gps_fix_t *out)
{
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_latest;
    xSemaphoreGive(s_lock);
    return true;
}
