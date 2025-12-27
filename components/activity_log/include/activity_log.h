#pragma once
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "sd_mmc_helper.h"

#ifdef __cplusplus
extern "C"
{
#endif

    // The row structure matches your 16 requested columns exactly
    typedef struct
    {
        // 1. RTC/Global Time
        time_t rtc_time;

        // 2. Session Time
        float session_time_s;

        // 3. Distance (Total for session)
        float total_distance_m;

        // 4. Instant Pace (s/500m)
        float pace_500m_s;

        // 5. SPM (Instant)
        float spm_instant;

        // 6. Average Pace (s/500m)
        float avg_pace_500m_s;

        // 7. Session Speed Avg (m/s)
        float avg_speed_mps;

        // 8. Stroke Length (m)
        float stroke_length_m;

        // 9. Stroke Count (Total)
        uint32_t stroke_count;

        // 10. GPS Lat
        double gps_lat;

        // 11. GPS Long
        double gps_lon;

        // 12. Power (W)
        float power_w;

        // 13. Drive Time (s)
        float drive_time_s;

        // 14. Recovery Time (s)
        float recovery_time_s;

        // 15. Recovery Ratio (Recovery / Drive)
        float recovery_ratio;

    } activity_log_row_t;

    typedef struct
    {
        bool opened;
        FILE *fp;
        char rel_path[96];
        uint32_t flush_every_n;
        uint32_t pending;
    } activity_log_t;

    esp_err_t activity_log_start(activity_log_t *log, sd_mmc_helper_t *sd, time_t start_ts, uint32_t activity_id);
    esp_err_t activity_log_append(activity_log_t *log, const activity_log_row_t *row);
    esp_err_t activity_log_stop(activity_log_t *log);

#ifdef __cplusplus
}
#endif