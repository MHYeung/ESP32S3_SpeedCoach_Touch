#include "ui_activity_detail_page.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "activity_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "ui_act_detail";

#include "storage_paths.h"
#define PAGE_ROWS 8

static uint32_t s_act_id = 0;
static char s_base_full[192] = {0}; // full base path WITHOUT suffix, e.g. "/sdcard/activities/20260104_2235_01"
static size_t s_page_index = 0;
static size_t s_total_rows = 0;
static size_t s_page_row_count = 0;
static bool s_total_known = false;
static bool s_has_next = false;
static bool s_pending_load = false;

typedef struct
{
    char base_full[192];
    size_t page_index;
    bool want_total;
} detail_load_req_t;

typedef struct
{
    char base_full[192];
    size_t page_index;
    size_t count;
    size_t total_rows;
    bool total_known;
    bool has_next;
    bool ok;
    bool retry_prev;
    bool interval_activity;
    uint16_t avg_hr;
    activity_store_split_t rows[PAGE_ROWS + 1];
} detail_load_result_t;

static QueueHandle_t s_load_q = NULL;
static TaskHandle_t s_load_task = NULL;
static bool s_loader_ready = false;

// UI handles
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_name_lbl = NULL; // activity name under status bar
static lv_obj_t *s_hdr_row = NULL;  // frozen header row container
static lv_obj_t *s_scroller = NULL; // content container holding table + nav
static lv_obj_t *s_tbl = NULL;      // split rows only (no header row inside)
static lv_obj_t *s_hint = NULL;     // optional hint when no splits
static lv_obj_t *s_nav_row = NULL;
static lv_obj_t *s_btn_prev = NULL;
static lv_obj_t *s_btn_next = NULL;
static lv_obj_t *s_page_lbl = NULL;
static ui_status_bar_t s_status;
static lv_obj_t *s_hdr_lbl[4] = {0};

static void request_load(void);

/*
 * Alternating row tint for the split table (LVGL v9).
 * Even rows use the theme surface colour; odd rows receive a subtle tint.
 * Must register LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS on the table object.
 */
static void tbl_row_tint_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    if (!t) return;
    if (lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
    if (!fill) return;
    if (fill->base.part != LV_PART_ITEMS) return;

    uint32_t row = fill->base.id1;
    if (row % 2 == 1) {
        /* Blend 10% black into the existing cell background for a subtle stripe */
        fill->color = lv_color_mix(lv_color_make(0, 0, 0), fill->color, 26);
    }
}

#define SPLIT_COLS 4
static const lv_coord_t s_col_w[SPLIT_COLS] = {24, 60, 74, 74}; // tweak to taste
static const char *s_col_hdr[SPLIT_COLS] = {"#", "Dist", "Time", "Pace"};
static const char *s_col_hdr_interval[SPLIT_COLS] = {"Rd", "Dist", "Time", "Pace"};

static void set_header_labels(bool interval_mode)
{
    const char *const *hdr = interval_mode ? s_col_hdr_interval : s_col_hdr;
    for (int c = 0; c < SPLIT_COLS; c++)
    {
        if (s_hdr_lbl[c])
            lv_label_set_text(s_hdr_lbl[c], hdr[c]);
    }
}

static void format_activity_title(char *out, size_t n)
{
    // default: show base filename (after last '/')
    const char *base = s_base_full;
    const char *p = strrchr(s_base_full, '/');
    if (p)
        base = p + 1;

    // try parse: YYYYMMDD_HHMM_XX  ->  YYYY-MM-DD HH:MM
    if (strlen(base) >= 13)
    {
        bool ok = true;
        for (int i = 0; i < 8; i++)
            if (base[i] < '0' || base[i] > '9')
                ok = false;
        if (base[8] != '_')
            ok = false;
        for (int i = 9; i < 13; i++)
            if (base[i] < '0' || base[i] > '9')
                ok = false;

        if (ok)
        {
            snprintf(out, n, "%.4s-%.2s-%.2s %.2s:%.2s",
                     base, base + 4, base + 6, base + 9, base + 11);
            return;
        }
    }

    snprintf(out, n, "%s", base);
}

static void trim_tail(char *s)
{
    size_t L = strlen(s);
    while (L && (s[L - 1] == '\r' || s[L - 1] == '\n' || s[L - 1] == ' '))
        s[--L] = 0;
}

static void normalize_base_full(const char *csv_path)
{
    s_base_full[0] = 0;
    if (!csv_path || !csv_path[0])
        return;

    // build full path first
    if (strncmp(csv_path, SD_MOUNT "/", strlen(SD_MOUNT "/")) == 0)
    {
        strncpy(s_base_full, csv_path, sizeof(s_base_full) - 1);
    }
    else
    {
        snprintf(s_base_full, sizeof(s_base_full), SD_MOUNT "/%s", csv_path);
    }
    trim_tail(s_base_full);

    // strip known suffixes if user passed strokes/splits csv
    const char *suffixes[] = {"/Strokes.csv", "/Splits.csv", "_Strokes.csv", "_Splits.csv", ".csv"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
    {
        size_t bl = strlen(s_base_full);
        size_t sl = strlen(suffixes[i]);
        if (bl > sl && strcmp(s_base_full + (bl - sl), suffixes[i]) == 0)
        {
            s_base_full[bl - sl] = 0;
        }
    }
}

static void show_hint(const char *msg)
{
    if (s_hint && lv_obj_is_valid(s_hint))
    {
        lv_label_set_text(s_hint, msg);
        lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_hint(void)
{
    if (s_hint && lv_obj_is_valid(s_hint))
    {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_btn_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn)
        return;
    if (enabled)
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else
        lv_obj_add_state(btn, LV_STATE_DISABLED);
}

static void update_nav_controls(void)
{
    if (s_page_lbl && lv_obj_is_valid(s_page_lbl))
    {
        char buf[24];
        if (s_total_known)
        {
            size_t total_pages = (s_total_rows + PAGE_ROWS - 1) / PAGE_ROWS;
            if (total_pages == 0)
                total_pages = 1;

            if (s_page_index >= total_pages)
                s_page_index = total_pages - 1;

            if (s_total_rows == 0)
                snprintf(buf, sizeof(buf), "0/0");
            else
                snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(s_page_index + 1), (unsigned)total_pages);
        }
        else
        {
            if (s_page_row_count == 0)
                snprintf(buf, sizeof(buf), "0/0");
            else
                snprintf(buf, sizeof(buf), "%u/?", (unsigned)(s_page_index + 1));
        }
        lv_label_set_text(s_page_lbl, buf);
    }

    if (s_total_known)
    {
        size_t total_pages = (s_total_rows + PAGE_ROWS - 1) / PAGE_ROWS;
        if (total_pages == 0)
            total_pages = 1;
        set_btn_enabled(s_btn_prev, s_total_rows > 0 && s_page_index > 0);
        set_btn_enabled(s_btn_next, s_total_rows > 0 && (s_page_index + 1) < total_pages);
    }
    else
    {
        set_btn_enabled(s_btn_prev, s_page_row_count > 0 && s_page_index > 0);
        set_btn_enabled(s_btn_next, s_page_row_count > 0 && s_has_next);
    }
}

static void format_split_time(char *out, size_t out_len, const char *in)
{
    if (!out || out_len == 0)
        return;
    out[0] = 0;
    if (!in || !in[0])
    {
        snprintf(out, out_len, "--:--.--");
        return;
    }

    int hh = 0, mm = 0, ss = 0, ms = 0;
    int n = sscanf(in, "%d:%d:%d.%d", &hh, &mm, &ss, &ms);
    if (n < 3)
    {
        snprintf(out, out_len, "%s", in);
        return;
    }
    if (n < 4)
        ms = 0;

    int total_min = hh * 60 + mm;
    int hundredths = ms / 10;
    snprintf(out, out_len, "%d:%02d.%02d", total_min, ss, hundredths);
}

static void apply_loaded_rows(const activity_store_split_t *rows,
                              size_t count,
                              bool has_next,
                              bool ok,
                              bool total_known,
                              size_t total_rows,
                              bool interval_activity)
{
    if (!s_tbl || !s_scroller)
        return;

    if (total_known)
    {
        s_total_known = true;
        s_total_rows = total_rows;
    }
    else if (!s_total_known)
    {
        s_total_rows = 0;
    }

    s_has_next = has_next;
    s_page_row_count = ok ? count : 0;

    bool interval_mode = interval_activity;
    if (count > 0)
        interval_mode = rows[0].is_interval;
    set_header_labels(interval_mode);

    if (!ok || count == 0)
    {
        lv_table_set_row_cnt(s_tbl, 0);
        if (interval_mode)
        {
            show_hint("No intervals yet.\nComplete a work/rest phase to log.");
        }
        else
        {
            show_hint("No splits in this file.");
        }
        update_nav_controls();
        return;
    }
    hide_hint();

    if (!s_total_known && !s_has_next && s_page_index == 0)
    {
        s_total_known = true;
        s_total_rows = count;
    }

    lv_table_set_row_cnt(s_tbl, (uint16_t)count);

    for (size_t i = 0; i < count; i++)
    {
        char c0[12], c1[16], c2[16];
        if (rows[i].is_interval && rows[i].label[0])
            snprintf(c0, sizeof(c0), "%s", rows[i].label);
        else
            snprintf(c0, sizeof(c0), "%lu", (unsigned long)rows[i].split_index);
        activity_store_format_dist(c1, sizeof(c1), rows[i].split_dist_m);
        format_split_time(c2, sizeof(c2), rows[i].split_time_str);

        lv_table_set_cell_value(s_tbl, (uint16_t)i, 0, c0);
        lv_table_set_cell_value(s_tbl, (uint16_t)i, 1, c1);
        lv_table_set_cell_value(s_tbl, (uint16_t)i, 2, c2);
        lv_table_set_cell_value(s_tbl, (uint16_t)i, 3, rows[i].pace_str);
    }

    lv_obj_set_style_border_side(s_tbl, LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_tbl, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(s_tbl, LV_OPA_60, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_tbl, lv_palette_main(LV_PALETTE_GREY), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_tbl, 0, LV_PART_ITEMS);

    lv_obj_set_height(s_tbl, LV_SIZE_CONTENT);
    lv_obj_update_layout(s_tbl);

    update_nav_controls();
}

static void apply_load_result_async(void *p)
{
    detail_load_result_t *res = (detail_load_result_t *)p;
    if (!res)
        return;

    if (strcmp(res->base_full, s_base_full) != 0 || res->page_index != s_page_index)
    {
        free(res);
        return;
    }

    if (res->retry_prev && s_page_index > 0)
    {
        s_page_index--;
        request_load();
        free(res);
        return;
    }

    apply_loaded_rows(res->rows,
                      res->count,
                      res->has_next,
                      res->ok,
                      res->total_known,
                      res->total_rows,
                      res->interval_activity);
    if (s_name_lbl && lv_obj_is_valid(s_name_lbl)) {
        char title[48];
        char buf[64];
        format_activity_title(title, sizeof(title));
        if (res->avg_hr > 0) {
            snprintf(buf, sizeof(buf), "%s  HR %u", title, (unsigned)res->avg_hr);
        } else {
            snprintf(buf, sizeof(buf), "%s", title);
        }
        lv_label_set_text(s_name_lbl, buf);
    }
    free(res);
}

static void detail_load_task(void *arg)
{
    (void)arg;
    detail_load_req_t req;

    for (;;)
    {
        if (xQueueReceive(s_load_q, &req, portMAX_DELAY) != pdTRUE)
            continue;

        detail_load_result_t *res = malloc(sizeof(*res));
        if (!res)
        {
            ESP_LOGE(TAG, "No memory for detail load");
            continue;
        }
        memset(res, 0, sizeof(*res));
        strncpy(res->base_full, req.base_full, sizeof(res->base_full) - 1);
        res->page_index = req.page_index;

        if (req.base_full[0] != 0)
        {
            size_t count = 0;
            size_t total = 0;
            activity_store_summary_t summary = {0};
            esp_err_t r = activity_store_load_splits_page(req.base_full,
                                                          req.page_index * PAGE_ROWS,
                                                          res->rows,
                                                          PAGE_ROWS + 1,
                                                          &count,
                                                          req.want_total ? &total : NULL,
                                                          &summary);

            if (r == ESP_OK && count == 0 && req.page_index > 0)
                res->retry_prev = true;

            if (count > PAGE_ROWS)
            {
                res->has_next = true;
                count = PAGE_ROWS;
            }

            res->count = count;
            res->ok = (r == ESP_OK && count > 0);
            if (req.want_total && total > 0)
            {
                res->total_known = true;
                res->total_rows = total;
            }
            res->interval_activity = summary.is_interval;
            res->avg_hr = summary.avg_hr;
        }

        lv_async_call(apply_load_result_async, res);
    }
}

static void ensure_loader(void)
{
    if (s_loader_ready)
        return;

    s_load_q = xQueueCreate(1, sizeof(detail_load_req_t));
    if (!s_load_q)
    {
        ESP_LOGE(TAG, "Failed to create detail load queue");
        return;
    }

    /* NimBLE sits on core 1. A pinned 6 KB stack there fails once both
     * sensor links are up. Leave the core free and keep the stack small. */
    BaseType_t ok = xTaskCreate(detail_load_task,
                                "act_detail_loader",
                                4096,
                                NULL,
                                4,
                                &s_load_task);
    if (ok != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create detail load task, free heap %u",
                 (unsigned)esp_get_free_heap_size());
        vQueueDelete(s_load_q);
        s_load_q = NULL;
        return;
    }

    s_loader_ready = true;
}

static void refresh_detail(void)
{
    if (!s_tbl || !s_scroller)
        return;
    if (s_base_full[0] == 0)
        return;

    activity_store_split_t rows[PAGE_ROWS + 1];
    memset(rows, 0, sizeof(rows));
    size_t count = 0;
    esp_err_t r = ESP_FAIL;
    bool has_next = false;
    activity_store_summary_t summary = {0};

    for (int attempt = 0; attempt < 2; attempt++)
    {
        r = activity_store_load_splits_page(s_base_full,
                                            s_page_index * PAGE_ROWS,
                                            rows,
                                            PAGE_ROWS + 1,
                                            &count,
                                            NULL,
                                            &summary);

        if (r == ESP_OK && count == 0 && s_page_index > 0)
        {
            s_page_index--;
            continue;
        }
        break;
    }

    if (count > PAGE_ROWS)
    {
        has_next = true;
        count = PAGE_ROWS;
    }

    apply_loaded_rows(rows,
                      count,
                      has_next,
                      (r == ESP_OK && count > 0),
                      false,
                      0,
                      summary.is_interval);
}

static void request_load(void)
{
    if (!s_tbl || !s_scroller)
    {
        s_pending_load = true;
        return;
    }
    s_pending_load = false;
    if (s_base_full[0] == 0)
        return;

    if (!s_total_known)
        s_total_rows = 0;
    s_has_next = false;
    s_page_row_count = 0;
    lv_table_set_row_cnt(s_tbl, 0);
    show_hint("Loading...");
    update_nav_controls();

    ensure_loader();
    if (!s_loader_ready)
    {
        refresh_detail();
        return;
    }

    detail_load_req_t req = {0};
    strncpy(req.base_full, s_base_full, sizeof(req.base_full) - 1);
    req.page_index = s_page_index;
    req.want_total = !s_total_known;
    xQueueOverwrite(s_load_q, &req);
}

static void nav_btn_event_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!id)
        return;

    if (strcmp(id, "prev") == 0)
    {
        if (s_page_index > 0)
            s_page_index--;
    }
    else if (strcmp(id, "next") == 0)
    {
        s_page_index++;
    }
    else
    {
        return;
    }

    request_load();
}

static void relayout(void)
{
    if (!s_root)
        return;

    bool land = ui_is_landscape();
    lv_coord_t w = lv_obj_get_width(s_root);

    lv_coord_t pad_lr = land ? 6 : 2;
    lv_coord_t hdr_h = 24;
    lv_coord_t nav_h = 32;

    if (s_name_lbl)
    {
        lv_obj_set_style_pad_left(s_name_lbl, 4, 0);
        lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_16, 0);
    }

    // Header row sizing
    if (s_hdr_row)
    {
        lv_obj_set_height(s_hdr_row, hdr_h);
        lv_obj_set_style_pad_left(s_hdr_row, pad_lr, 0);
        lv_obj_set_style_pad_right(s_hdr_row, pad_lr, 0);
        lv_obj_set_style_pad_top(s_hdr_row, 1, 0);
        lv_obj_set_style_pad_bottom(s_hdr_row, 1, 0);
    }

    if (s_hint)
    {
        lv_obj_set_width(s_hint, land ? lv_pct(70) : lv_pct(90));
        lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_hint);
    }

    if (s_nav_row)
    {
        lv_obj_set_height(s_nav_row, nav_h);
        lv_obj_set_style_pad_left(s_nav_row, pad_lr, 0);
        lv_obj_set_style_pad_right(s_nav_row, pad_lr, 0);
    }

    if (s_btn_prev)
        lv_obj_set_size(s_btn_prev, land ? 36 : 40, nav_h);
    if (s_btn_next)
        lv_obj_set_size(s_btn_next, land ? 36 : 40, nav_h);

    // Column widths depend on screen width/orientation
    lv_coord_t usable = w - (pad_lr * 2);

    lv_coord_t c1 = land ? 75 : 50;        // Dist
    lv_coord_t c2 = land ? 96 : 76;        // Time
    lv_coord_t c3 = land ? 96 : 76;        // Pace
    lv_coord_t c0 = usable - c3 - c1 - c2; // #

    // Guard minimum for last column
    lv_coord_t c3_min = land ? 90 : 70;
    if (c3 < c3_min)
    {
        lv_coord_t need = c3_min - c3;
        // steal from c2 first, then c1
        lv_coord_t steal2 = (c2 > (land ? 80 : 60)) ? (need > 20 ? 20 : need) : 0;
        c2 -= steal2;
        need -= steal2;
        lv_coord_t steal1 = (need > 0 && c1 > (land ? 60 : 44)) ? need : 0;
        c1 -= steal1;
        need -= steal1;
        c3 = usable - c0 - c1 - c2;
    }

    // Apply header label widths
    if (s_hdr_lbl[0])
        lv_obj_set_width(s_hdr_lbl[0], c0);
    if (s_hdr_lbl[1])
        lv_obj_set_width(s_hdr_lbl[1], c1);
    if (s_hdr_lbl[2])
        lv_obj_set_width(s_hdr_lbl[2], c2);
    if (s_hdr_lbl[3])
        lv_obj_set_width(s_hdr_lbl[3], c3);

    // Apply table column widths
    if (s_tbl)
    {
        lv_table_set_col_cnt(s_tbl, 4);
        lv_table_set_col_width(s_tbl, 0, c0);
        lv_table_set_col_width(s_tbl, 1, c1);
        lv_table_set_col_width(s_tbl, 2, c2);
        lv_table_set_col_width(s_tbl, 3, c3);
    }

    // Ensure hint is centered after rotation
    if (s_hint)
        lv_obj_center(s_hint);
}

void activity_detail_page_open(uint32_t activity_id, const char *csv_path)
{
    s_act_id = activity_id;
    normalize_base_full(csv_path);
    s_page_index = 0;
    s_total_rows = 0;
    s_total_known = false;
    s_has_next = false;
    s_page_row_count = 0;

    if (s_name_lbl && lv_obj_is_valid(s_name_lbl))
    {
        char title[48];
        format_activity_title(title, sizeof(title));
        lv_label_set_text(s_name_lbl, title);
    }

    ESP_LOGI(TAG, "Open detail: id=%lu base=%s", (unsigned long)s_act_id, s_base_full);

    request_load();
}

void activity_detail_page_create(lv_obj_t *parent)
{
    // page background (must cover what's behind)
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    ui_theme_apply_screen(parent);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Root: compact status -> title+pager -> header -> split table
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    ui_status_bar_create(&s_status, s_root);
    ui_status_bar_set_title(&s_status, "Splits", UI_ACTIVITY_SUMMARY_PAGE);

    s_nav_row = lv_obj_create(s_root);
    lv_obj_set_width(s_nav_row, lv_pct(100));
    lv_obj_set_height(s_nav_row, 22);
    lv_obj_set_style_bg_opa(s_nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_nav_row, 0, 0);
    lv_obj_set_style_pad_all(s_nav_row, 0, 0);
    lv_obj_set_style_pad_left(s_nav_row, 4, 0);
    lv_obj_set_style_pad_right(s_nav_row, 4, 0);
    lv_obj_clear_flag(s_nav_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_name_lbl = lv_label_create(s_nav_row);
    ui_theme_apply_label(s_name_lbl, false);
    lv_obj_set_flex_grow(s_name_lbl, 1);
    lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_name_lbl, "Activity");

    s_btn_prev = lv_btn_create(s_nav_row);
    ui_theme_apply_button(s_btn_prev);
    lv_obj_add_event_cb(s_btn_prev, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)"prev");
    lv_obj_t *prev_lbl = lv_label_create(s_btn_prev);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, ui_theme_palette()->accent_text, 0);
    lv_obj_align(prev_lbl, LV_ALIGN_CENTER, 0, 0);

    s_page_lbl = lv_label_create(s_nav_row);
    ui_theme_apply_label(s_page_lbl, true);
    lv_obj_set_style_text_font(s_page_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_hor(s_page_lbl, 4, 0);
    lv_label_set_text(s_page_lbl, "0/0");

    s_btn_next = lv_btn_create(s_nav_row);
    ui_theme_apply_button(s_btn_next);
    lv_obj_add_event_cb(s_btn_next, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)"next");
    lv_obj_t *next_lbl = lv_label_create(s_btn_next);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, ui_theme_palette()->accent_text, 0);
    lv_obj_align(next_lbl, LV_ALIGN_CENTER, 0, 0);

    s_hdr_row = lv_obj_create(s_root);
    lv_obj_set_width(s_hdr_row, lv_pct(100));
    lv_obj_set_height(s_hdr_row, 18);
    lv_obj_set_style_border_width(s_hdr_row, 0, 0);
    lv_obj_set_style_pad_left(s_hdr_row, 4, 0);
    lv_obj_set_style_pad_right(s_hdr_row, 4, 0);
    lv_obj_set_style_pad_top(s_hdr_row, 0, 0);
    lv_obj_set_style_pad_bottom(s_hdr_row, 0, 0);
    ui_theme_apply_surface(s_hdr_row); // looks like a header strip
    lv_obj_clear_flag(s_hdr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hdr_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int c = 0; c < SPLIT_COLS; c++)
    {
        s_hdr_lbl[c] = lv_label_create(s_hdr_row);
        ui_theme_apply_label(s_hdr_lbl[c], false);
        lv_label_set_text(s_hdr_lbl[c], s_col_hdr[c]);
        lv_obj_set_width(s_hdr_lbl[c], s_col_w[c]);
        lv_obj_set_flex_grow(s_hdr_lbl[c], 0);
        lv_obj_set_style_text_align(s_hdr_lbl[c], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(s_hdr_lbl[c], &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(s_hdr_lbl[c], LV_LABEL_LONG_CLIP);
    }

    // Content area (paged, non-scrollable)
    s_scroller = lv_obj_create(s_root);
    lv_obj_set_width(s_scroller, lv_pct(100));
    lv_obj_set_flex_grow(s_scroller, 1);
    lv_obj_set_style_bg_opa(s_scroller, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroller, 0, 0);
    lv_obj_set_style_pad_all(s_scroller, 0, 0);
    lv_obj_set_flex_flow(s_scroller, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scroller, 4, 0);
    lv_obj_add_flag(s_scroller, LV_OBJ_FLAG_SCROLLABLE);

    // Table inside content area
    s_tbl = lv_table_create(s_scroller);
    lv_obj_set_width(s_tbl, lv_pct(100));
    lv_obj_set_height(s_tbl, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_tbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_tbl, LV_SCROLLBAR_MODE_OFF);

    lv_table_set_col_cnt(s_tbl, SPLIT_COLS);
    for (int c = 0; c < SPLIT_COLS; c++)
    {
        lv_table_set_col_width(s_tbl, c, s_col_w[c]);
    }

    lv_obj_set_style_pad_hor(s_tbl, 2, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(s_tbl, 6, LV_PART_ITEMS);
    lv_obj_set_style_text_align(s_tbl, LV_TEXT_ALIGN_CENTER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_tbl, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_tbl, ui_theme_palette()->text, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_tbl, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_tbl, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_tbl, ui_theme_palette()->border, LV_PART_ITEMS);
    lv_obj_set_style_border_opa(s_tbl, LV_OPA_COVER, LV_PART_ITEMS);

    /* Explicit item background so the FILL draw task is always emitted */
    lv_obj_set_style_bg_opa(s_tbl, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tbl, ui_theme_palette()->surface, LV_PART_ITEMS);

    /* Alternating row tint via draw-task hook (LVGL v9 API) */
    lv_obj_add_flag(s_tbl, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(s_tbl, tbl_row_tint_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);

    // Hint label (shown when no splits)
    s_hint = lv_label_create(s_scroller);
    ui_theme_apply_label(s_hint, true);
    lv_label_set_text(s_hint, "");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_hint);

    // start empty
    lv_table_set_row_cnt(s_tbl, 0);

    relayout();
    update_nav_controls();

    if (s_pending_load && s_base_full[0] != 0)
    {
        s_pending_load = false;
        request_load();
    }
}

void activity_detail_page_apply_theme(void)
{
    if (!s_root)
        return;
    ui_status_bar_apply_theme(&s_status);
    if (s_hdr_row)
        ui_theme_apply_surface(s_hdr_row);
    if (s_btn_prev) {
        ui_theme_apply_button(s_btn_prev);
        lv_obj_t *glyph = lv_obj_get_child(s_btn_prev, 0);
        if (glyph)
            lv_obj_set_style_text_color(glyph, ui_theme_palette()->accent_text, 0);
    }
    if (s_btn_next) {
        ui_theme_apply_button(s_btn_next);
        lv_obj_t *glyph = lv_obj_get_child(s_btn_next, 0);
        if (glyph)
            lv_obj_set_style_text_color(glyph, ui_theme_palette()->accent_text, 0);
    }
    if (s_page_lbl)
        ui_theme_apply_label(s_page_lbl, true);
    if (s_name_lbl)
        ui_theme_apply_label(s_name_lbl, false);
    if (s_tbl) {
        lv_obj_set_style_bg_color(s_tbl, ui_theme_palette()->surface, LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_tbl, ui_theme_palette()->text, LV_PART_ITEMS);
        lv_obj_set_style_border_color(s_tbl, ui_theme_palette()->border, LV_PART_ITEMS);
    }
}

void activity_detail_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_status);
    relayout();
}
