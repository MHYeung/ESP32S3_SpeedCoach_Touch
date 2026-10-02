#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t split_index;
    float total_dist_m;
    float split_dist_m;
    char split_time_str[20];   // "HH:MM:SS.mmm"
    char pace_str[16];         // "MM:SS.s" or "--:--.-"
    float avg_spm;
    bool is_interval;
    char label[12];
    char phase[6];             // "WORK" or "REST"
    uint32_t target_value;
    char target_unit[8];       // "s", "m", "st"
} activity_store_split_t;

typedef struct {
    uint32_t activity_id;
    time_t start_ts;           // 0 if not parsed
    float split_setting_m;     // 0 if not parsed

    float total_distance_m;    // derived from last split row
    float total_time_s;        // sum(split_time)
    float avg_pace_s_per500;   // derived: total_time / (total_dist/500)
    bool is_interval;
    uint16_t avg_hr;           /* 0 when the log has no heart-rate samples */
} activity_store_summary_t;

esp_err_t activity_store_resolve_paths(const char *in_path_or_base,
                                       char *out_strokes, size_t strokes_len,
                                       char *out_splits, size_t splits_len);

esp_err_t activity_store_load_splits(const char *in_path_or_base,
                                     activity_store_split_t *out_rows,
                                     size_t max_rows,
                                     size_t *out_count,
                                     activity_store_summary_t *out_summary);

esp_err_t activity_store_load_splits_page(const char *in_path_or_base,
                                          size_t start_index,
                                          activity_store_split_t *out_rows,
                                          size_t max_rows,
                                          size_t *out_count,
                                          size_t *out_total_count,
                                          activity_store_summary_t *out_summary);

void activity_store_format_dist(char *out, size_t out_len, float meters);
void activity_store_format_pace_500(char *out, size_t out_len, float pace_s_per500);

#ifdef __cplusplus
}
#endif
