#include "activity_store.h"
#include "storage_paths.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "activity_store";

static bool ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return false;
    return (memcmp(s + (ls - lf), suffix, lf) == 0);
}

static bool file_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0);
}

/* Heart rate is the field after `commas` commas. Blank or absent is 0. */
static uint16_t hr_after_commas(const char *s, int commas)
{
    const char *p = s;
    for (int i = 0; i < commas && p; i++) {
        p = strchr(p, ',');
        if (!p) {
            return 0;
        }
        p++;
    }
    if (!p || *p == '\0') {
        return 0;
    }
    unsigned v = 0;
    if (sscanf(p, "%u", &v) != 1 || v > 250) {
        return 0;
    }
    return (uint16_t)v;
}

static bool resolve_dir_layout(const char *dir,
                               char *out_strokes, size_t strokes_len,
                               char *out_splits, size_t splits_len)
{
    if (!dir || !dir[0]) return false;

    char strokes[256];
    char splits[256];
    if (snprintf(strokes, sizeof(strokes), "%s/Strokes.csv", dir) >= (int)sizeof(strokes)) return false;
    if (snprintf(splits, sizeof(splits), "%s/Splits.csv", dir) >= (int)sizeof(splits)) return false;

    if (!file_exists(strokes) && !file_exists(splits)) return false;

    if (out_strokes) snprintf(out_strokes, strokes_len, "%s", strokes);
    if (out_splits) snprintf(out_splits, splits_len, "%s", splits);
    return true;
}

static bool resolve_old_layout(const char *base,
                               char *out_strokes, size_t strokes_len,
                               char *out_splits, size_t splits_len)
{
    if (!base || !base[0]) return false;

    char old_strokes[256];
    char old_splits[256];
    if (snprintf(old_strokes, sizeof(old_strokes), "%s_Strokes.csv", base) >= (int)sizeof(old_strokes)) return false;
    if (snprintf(old_splits, sizeof(old_splits), "%s_Splits.csv", base) >= (int)sizeof(old_splits)) return false;

    if (!file_exists(old_strokes) && !file_exists(old_splits)) return false;

    if (out_strokes) snprintf(out_strokes, strokes_len, "%s", old_strokes);
    if (out_splits) snprintf(out_splits, splits_len, "%s", old_splits);
    return true;
}

static void strip_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = 0;
        n--;
    }
}

static const char* skip_ws(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool parse_hms_ms(const char *s, float *out_s) {
    // "HH:MM:SS.mmm"
    if (!s || !out_s) return false;
    int hh=0, mm=0, ss=0, ms=0;
    int n = sscanf(s, "%d:%d:%d.%d", &hh, &mm, &ss, &ms);
    if (n < 3) return false;
    if (n < 4) ms = 0;
    *out_s = (float)(hh*3600 + mm*60 + ss) + (float)ms / 1000.0f;
    return true;
}

static void build_interval_label(char *out, size_t out_len, uint32_t round_idx, const char *phase)
{
    if (!out || out_len == 0)
        return;
    char tag = 'W';
    if (phase && (phase[0] == 'R' || phase[0] == 'r'))
        tag = 'R';
    snprintf(out, out_len, "%lu%c", (unsigned long)round_idx, tag);
}

void activity_store_format_dist(char *out, size_t out_len, float meters) {
    if (!out || out_len == 0) return;
    if (meters < 0.1f) {
        snprintf(out, out_len, "--");
    } else if (meters >= 1000.0f) {
        snprintf(out, out_len, "%.2f km", (double)(meters / 1000.0f));
    } else {
        snprintf(out, out_len, "%.0f m", (double)meters);
    }
}

void activity_store_format_pace_500(char *out, size_t out_len, float pace_s_per500) {
    if (!out || out_len == 0) return;
    if (pace_s_per500 <= 0.1f || pace_s_per500 > 3600.0f) {
        snprintf(out, out_len, "--:--/500");
        return;
    }
    uint32_t sec = (uint32_t)(pace_s_per500 + 0.5f);
    uint32_t mm = sec / 60;
    uint32_t ss = sec % 60;
    snprintf(out, out_len, "%lu:%02lu/500", (unsigned long)mm, (unsigned long)ss);
}

esp_err_t activity_store_resolve_paths(const char *in_path_or_base,
                                       char *out_strokes, size_t strokes_len,
                                       char *out_splits, size_t splits_len)
{
    if (!in_path_or_base || !in_path_or_base[0]) return ESP_ERR_INVALID_ARG;

    char tmp[256];
    memset(tmp, 0, sizeof(tmp));

    // Make it absolute if needed
    if (in_path_or_base[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", in_path_or_base);
    } else {
        snprintf(tmp, sizeof(tmp), SD_MOUNT "/%s", in_path_or_base);
    }

    // If user passed a base (no .csv), resolve to new or old layout
    if (!ends_with(tmp, ".csv")) {
        if (resolve_dir_layout(tmp, out_strokes, strokes_len, out_splits, splits_len)) {
            return ESP_OK;
        }
        if (resolve_old_layout(tmp, out_strokes, strokes_len, out_splits, splits_len)) {
            return ESP_OK;
        }

        // If the base accidentally includes duplicate "<name>/<name>", try dropping the last segment.
        const char *last_slash = strrchr(tmp, '/');
        if (last_slash && last_slash[1] != '\0') {
            const char *prev_slash = NULL;
            for (const char *p = last_slash - 1; p > tmp; --p) {
                if (*p == '/') { prev_slash = p; break; }
            }
            if (prev_slash) {
                size_t last_len = strlen(last_slash + 1);
                size_t prev_len = (size_t)(last_slash - prev_slash - 1);
                if (prev_len == last_len && memcmp(prev_slash + 1, last_slash + 1, last_len) == 0) {
                    char dedup[256];
                    size_t keep = (size_t)(last_slash - tmp);
                    if (keep < sizeof(dedup)) {
                        memcpy(dedup, tmp, keep);
                        dedup[keep] = 0;
                        if (resolve_dir_layout(dedup, out_strokes, strokes_len, out_splits, splits_len)) {
                            return ESP_OK;
                        }
                    }
                }
            }
        }

        // As a fallback, map ".../activities/<name>" to ".../activities/<name>/<name>"
        // for older callers that expect the duplicate base form.
        if (last_slash && last_slash[1] != '\0') {
            const char *last = last_slash + 1;
            size_t need = strlen(tmp) + 1 + strlen(last) + 1;
            if (need <= sizeof(tmp)) {
                strcat(tmp, "/");
                strcat(tmp, last);
                if (resolve_dir_layout(tmp, out_strokes, strokes_len, out_splits, splits_len)) {
                    return ESP_OK;
                }
                if (resolve_old_layout(tmp, out_strokes, strokes_len, out_splits, splits_len)) {
                    return ESP_OK;
                }
            }
        }

        // Final fallback: assume old suffix naming even if files don't exist yet.
        if (out_strokes) snprintf(out_strokes, strokes_len, "%s_Strokes.csv", tmp);
        if (out_splits)  snprintf(out_splits,  splits_len,  "%s_Splits.csv",  tmp);
        return ESP_OK;
    }

    // If user passed a specific CSV, derive the other if possible
    if (ends_with(tmp, "/Strokes.csv")) {
        if (out_strokes) snprintf(out_strokes, strokes_len, "%s", tmp);
        if (out_splits) {
            char base[256];
            snprintf(base, sizeof(base), "%s", tmp);
            base[strlen(base) - strlen("/Strokes.csv")] = 0;
            snprintf(out_splits, splits_len, "%s/Splits.csv", base);
        }
        return ESP_OK;
    }

    if (ends_with(tmp, "/Splits.csv")) {
        if (out_splits) snprintf(out_splits, splits_len, "%s", tmp);
        if (out_strokes) {
            char base[256];
            snprintf(base, sizeof(base), "%s", tmp);
            base[strlen(base) - strlen("/Splits.csv")] = 0;
            snprintf(out_strokes, strokes_len, "%s/Strokes.csv", base);
        }
        return ESP_OK;
    }

    if (ends_with(tmp, "_Strokes.csv")) {
        if (out_strokes) snprintf(out_strokes, strokes_len, "%s", tmp);
        if (out_splits) {
            char base[256];
            snprintf(base, sizeof(base), "%s", tmp);
            base[strlen(base) - strlen("_Strokes.csv")] = 0;
            snprintf(out_splits, splits_len, "%s_Splits.csv", base);
        }
        return ESP_OK;
    }

    if (ends_with(tmp, "_Splits.csv")) {
        if (out_splits) snprintf(out_splits, splits_len, "%s", tmp);
        if (out_strokes) {
            char base[256];
            snprintf(base, sizeof(base), "%s", tmp);
            base[strlen(base) - strlen("_Splits.csv")] = 0;
            snprintf(out_strokes, strokes_len, "%s_Strokes.csv", base);
        }
        return ESP_OK;
    }

    // Unknown .csv name: just return it as "splits" and leave strokes empty
    if (out_splits)  snprintf(out_splits,  splits_len,  "%s", tmp);
    if (out_strokes) snprintf(out_strokes, strokes_len, "%s", "");
    return ESP_OK;
}

esp_err_t activity_store_load_splits_page(const char *in_path_or_base,
                                          size_t start_index,
                                          activity_store_split_t *out_rows,
                                          size_t max_rows,
                                          size_t *out_count,
                                          size_t *out_total_count,
                                          activity_store_summary_t *out_summary)
{
    if (out_count) *out_count = 0;
    if (out_total_count) *out_total_count = 0;
    if (out_summary) memset(out_summary, 0, sizeof(*out_summary));

    char strokes_path[256], splits_path[256];
    memset(strokes_path, 0, sizeof(strokes_path));
    memset(splits_path, 0, sizeof(splits_path));

    esp_err_t r = activity_store_resolve_paths(in_path_or_base,
                                               strokes_path, sizeof(strokes_path),
                                               splits_path, sizeof(splits_path));
    if (r != ESP_OK) return r;

    FILE *f = fopen(splits_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open splits: %s", splits_path);
        return ESP_FAIL;
    }

    char line[256];
    bool in_data = false;
    bool interval_data = false;

    size_t stored = 0;
    size_t seen = 0;
    float total_time_s = 0.0f;
    float last_total_dist = 0.0f;
    float cumulative_dist = 0.0f;
    uint32_t hr_sum = 0;
    uint16_t hr_n = 0;

    const bool fast_page_only = (out_total_count == NULL && out_summary == NULL);

    while (fgets(line, sizeof(line), f)) {
        strip_newline(line);
        const char *p = skip_ws(line);
        if (!p || !p[0]) continue;

        if (!in_data) {
            // Find the column header row. Everything before is metadata.
            if (strncmp(p, "Split #", 7) == 0) {
                in_data = true;
                interval_data = false;
                if (out_summary) out_summary->is_interval = false;
                continue;
            }
            if (strncmp(p, "Round,Phase", 11) == 0 || strncmp(p, "Round, Phase", 12) == 0) {
                in_data = true;
                interval_data = true;
                if (out_summary) out_summary->is_interval = true;
                continue;
            }
            // Optional metadata parsing (safe to ignore if you don't need it)
            // e.g. "Split Setting,500 meters"
            if (out_summary && strncmp(p, "Split Setting,", 14) == 0) {
                // parse first float found in the string
                float v = 0.0f;
                if (sscanf(p, "Split Setting,%f", &v) == 1) out_summary->split_setting_m = v;
            }
            if (out_summary && strncmp(p, "Activity Type,", 14) == 0) {
                const char *t = p + 14;
                t = skip_ws(t);
                if (t && (strstr(t, "Interval") || strstr(t, "Intervals")))
                    out_summary->is_interval = true;
            }
            if (out_summary && strncmp(p, "Activity ID,", 12) == 0) {
                unsigned long id = 0;
                if (sscanf(p, "Activity ID,%lu", &id) == 1) out_summary->activity_id = (uint32_t)id;
            }
            continue;
        }

        // Data rows:
        // Split #,Total Dist (m),Split Dist (m),Split Time,Avg Pace (/500m),Avg SPM
        // Round,Phase,Target,Unit,Phase Time,Phase Dist (m),Phase Pace (/500m),Avg SPM
        activity_store_split_t row = {0};
        if (!interval_data)
        {
            unsigned long idx = 0;
            double total_d = 0, split_d = 0, spm = 0;
            char tstr[20] = {0};
            char pstr[16] = {0};

            int n = sscanf(p, "%lu,%lf,%lf,%19[^,],%15[^,],%lf",
                           &idx, &total_d, &split_d, tstr, pstr, &spm);
            if (n < 5) continue;

            row.split_index = (uint32_t)idx;
            row.total_dist_m = (float)total_d;
            row.split_dist_m = (float)split_d;
            strncpy(row.split_time_str, tstr, sizeof(row.split_time_str) - 1);
            strncpy(row.pace_str, pstr, sizeof(row.pace_str) - 1);
            row.avg_spm = (n >= 6) ? (float)spm : 0.0f;
            {
                uint16_t hr = hr_after_commas(p, 6);
                if (hr > 0 && hr_n < UINT16_MAX) {
                    hr_sum += hr;
                    hr_n++;
                }
            }
            row.is_interval = false;
            snprintf(row.label, sizeof(row.label), "%lu", (unsigned long)row.split_index);

            float dt = 0.0f;
            if (parse_hms_ms(row.split_time_str, &dt)) {
                total_time_s += dt;
            }
            last_total_dist = row.total_dist_m;
        }
        else
        {
            unsigned long round_idx = 0;
            unsigned long target = 0;
            double phase_d = 0.0;
            double spm = 0.0;
            char phase[6] = {0};
            char unit[8] = {0};
            char tstr[20] = {0};
            char pstr[16] = {0};

            int n = sscanf(p, "%lu,%5[^,],%lu,%7[^,],%19[^,],%lf,%15[^,],%lf",
                           &round_idx, phase, &target, unit, tstr, &phase_d, pstr, &spm);
            if (n < 6) continue;

            row.split_index = (uint32_t)round_idx;
            row.split_dist_m = (float)phase_d;
            cumulative_dist += row.split_dist_m;
            row.total_dist_m = cumulative_dist;
            strncpy(row.split_time_str, tstr, sizeof(row.split_time_str) - 1);
            strncpy(row.pace_str, pstr, sizeof(row.pace_str) - 1);
            row.avg_spm = (n >= 8) ? (float)spm : 0.0f;
            {
                uint16_t hr = hr_after_commas(p, 8);
                if (hr > 0 && hr_n < UINT16_MAX) {
                    hr_sum += hr;
                    hr_n++;
                }
            }
            row.is_interval = true;
            strncpy(row.phase, phase, sizeof(row.phase) - 1);
            row.target_value = (uint32_t)target;
            strncpy(row.target_unit, unit, sizeof(row.target_unit) - 1);
            build_interval_label(row.label, sizeof(row.label), row.split_index, row.phase);

            float dt = 0.0f;
            if (parse_hms_ms(row.split_time_str, &dt)) {
                total_time_s += dt;
            }
            last_total_dist = row.total_dist_m;
        }

        if (seen >= start_index && out_rows && stored < max_rows) {
            out_rows[stored++] = row;
        }
        seen++;

        // If caller doesn't need total/summary, stop once we have enough rows.
        if (fast_page_only && stored >= max_rows) {
            break;
        }
    }

    fclose(f);

    if (out_count) *out_count = stored;
    if (out_total_count) *out_total_count = seen;

    if (out_summary) {
        out_summary->total_distance_m = last_total_dist;
        out_summary->total_time_s = total_time_s;
        if (last_total_dist > 0.1f) {
            out_summary->avg_pace_s_per500 = total_time_s / (last_total_dist / 500.0f);
        } else {
            out_summary->avg_pace_s_per500 = 0.0f;
        }
        out_summary->avg_hr = (hr_n > 0) ? (uint16_t)(hr_sum / hr_n) : 0;
    }

    // If file had data but we stored 0 (max_rows=0) still OK.
    return (seen > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t activity_store_load_splits(const char *in_path_or_base,
                                     activity_store_split_t *out_rows,
                                     size_t max_rows,
                                     size_t *out_count,
                                     activity_store_summary_t *out_summary)
{
    return activity_store_load_splits_page(in_path_or_base,
                                           0,
                                           out_rows,
                                           max_rows,
                                           out_count,
                                           NULL,
                                           out_summary);
}
