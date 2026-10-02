// main/ui/ui_activity_summary_page.c
#include "ui_activity_summary_page.h"
#include "ui_activity_detail_page.h"
#include "ui.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *TAG = "ui_act_sum";

#include "storage_paths.h"

typedef struct
{
    uint32_t id;
    time_t start_ts;
    uint32_t duration_s;
    float distance_m;
    float avg_pace_s_per500;
    char file_path[160];
} activity_meta_t;

typedef struct
{
    uint32_t id;
    char path[160];
} open_arg_t;

#define ACT_MAX 128
#define PAGE_ROWS 10
static activity_meta_t s_items[ACT_MAX];
static size_t s_item_count = 0;
static size_t s_page_index = 0;
static size_t s_total_pages = 1;

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_status;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_empty_lbl = NULL;
static lv_obj_t *s_nav_row = NULL;
static lv_obj_t *s_btn_prev = NULL;
static lv_obj_t *s_btn_next = NULL;
static lv_obj_t *s_page_lbl = NULL;

static int cmp_activity_newest_first(const void *a, const void *b)
{
    const activity_meta_t *A = (const activity_meta_t *)a;
    const activity_meta_t *B = (const activity_meta_t *)b;

    if (A->start_ts < B->start_ts)
        return 1;
    if (A->start_ts > B->start_ts)
        return -1;

    // tie-breaker (newer id first)
    if (A->id < B->id)
        return 1;
    if (A->id > B->id)
        return -1;
    return 0;
}

static void fmt_time_hms(char *out, size_t n, uint32_t sec)
{
    uint32_t h = sec / 3600;
    uint32_t m = (sec % 3600) / 60;
    uint32_t s = sec % 60;
    snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

static void fmt_pace_500(char *out, size_t n, float pace_s_per500)
{
    if (pace_s_per500 <= 0.1f)
    {
        snprintf(out, n, "--:--/500");
        return;
    }
    uint32_t sec = (uint32_t)(pace_s_per500 + 0.5f);
    uint32_t mm = sec / 60;
    uint32_t ss = sec % 60;
    snprintf(out, n, "%lu:%02lu/500", (unsigned long)mm, (unsigned long)ss);
}

static void fmt_dist(char *out, size_t n, float m)
{
    if (m < 0.1f)
    {
        snprintf(out, n, "--");
        return;
    }
    if (m >= 1000.0f)
        snprintf(out, n, "%.2f km", (double)(m / 1000.0f));
    else
        snprintf(out, n, "%.0f m", (double)m);
}

static bool load_index_file(void)
{
    s_item_count = 0;

    FILE *f = fopen(ACTIVITY_INDEX_PATH, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "index not found: %s", ACTIVITY_INDEX_PATH);
        return false;
    }

    char line[256];

    // Optional header line: skip if it contains non-numeric at start
    long pos = ftell(f);
    if (fgets(line, sizeof(line), f))
    {
        if (!(line[0] >= '0' && line[0] <= '9'))
        {
            // header; keep going
        }
        else
        {
            fseek(f, pos, SEEK_SET); // not header
        }
    }
    else
    {
        fclose(f);
        return false;
    }

    while (fgets(line, sizeof(line), f) && s_item_count < ACT_MAX)
    {
        // id,start_ts,duration_s,distance_m,avg_pace_s_per500,file_path
        activity_meta_t *it = &s_items[s_item_count];
        memset(it, 0, sizeof(*it));

        // file_path may contain slashes; read it as the remainder after the 5th comma
        char path[160] = {0};
        unsigned long id = 0, start_ts = 0, dur = 0;
        double dist = 0, pace = 0;

        // parse first 5 fields
        int n = sscanf(line, "%lu,%lu,%lu,%lf,%lf,%159[^\n]",
                       &id, &start_ts, &dur, &dist, &pace, path);

        if (n < 6)
            continue;

        it->id = (uint32_t)id;
        it->start_ts = (time_t)start_ts;
        it->distance_m = (float)dist;
        it->duration_s = (uint32_t)dur;

        // keep the raw field for now; we won't display pace on summary tiles
        it->avg_pace_s_per500 = (float)pace;

        strncpy(it->file_path, path, sizeof(it->file_path) - 1);

        // Trim CR/LF/spaces (Windows line endings can break paths)
        size_t L = strlen(it->file_path);
        while (L && (it->file_path[L - 1] == '\r' || it->file_path[L - 1] == '\n' || it->file_path[L - 1] == ' '))
        {
            it->file_path[--L] = 0;
        }

        // Backward-compat: if duration looks like ms, convert to seconds
        if (it->duration_s > 86400UL * 10UL)
        { // >10 days -> almost certainly ms
            it->duration_s /= 1000;
        }

        // Backward-compat: if "pace" field was actually speed (m/s), convert to pace sec/500
        if (it->avg_pace_s_per500 > 0.01f && it->avg_pace_s_per500 < 30.0f)
        {
            it->avg_pace_s_per500 = 500.0f / it->avg_pace_s_per500;
        }

        s_item_count++;
    }

    qsort(s_items, s_item_count, sizeof(s_items[0]), cmp_activity_newest_first);

    fclose(f);
    return (s_item_count > 0);
}

static open_arg_t s_open_arg;

static void open_detail_async(void *p)
{
    open_arg_t *a = (open_arg_t *)p;
    activity_detail_page_open(a->id, a->path);
}

static void build_list_ui(void);

static void activity_card_cb(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_item_count)
        return;
    activity_meta_t *it = &s_items[idx];

    ui_go_to_page(UI_ACTIVITY_DETAIL_PAGE, true);

    s_open_arg.id = it->id;
    strncpy(s_open_arg.path, it->file_path, sizeof(s_open_arg.path) - 1);
    lv_async_call(open_detail_async, &s_open_arg);
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
    if (s_item_count == 0)
        s_total_pages = 1;
    else
        s_total_pages = (s_item_count + PAGE_ROWS - 1) / PAGE_ROWS;

    if (s_page_index >= s_total_pages)
        s_page_index = s_total_pages - 1;

    if (s_page_lbl && lv_obj_is_valid(s_page_lbl))
    {
        char buf[24];
        if (s_item_count == 0)
            snprintf(buf, sizeof(buf), "0/0");
        else
            snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(s_page_index + 1), (unsigned)s_total_pages);
        lv_label_set_text(s_page_lbl, buf);
    }

    set_btn_enabled(s_btn_prev, s_item_count > 0 && s_page_index > 0);
    set_btn_enabled(s_btn_next, s_item_count > 0 && (s_page_index + 1) < s_total_pages);
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
        if ((s_page_index + 1) < s_total_pages)
            s_page_index++;
    }
    else
    {
        return;
    }

    build_list_ui();
}

static void build_list_ui(void)
{
    if (!s_list || !lv_obj_is_valid(s_list))
        return;

    lv_obj_clean(s_list);
    if (s_empty_lbl && lv_obj_is_valid(s_empty_lbl))
    {
        lv_obj_del(s_empty_lbl);
        s_empty_lbl = NULL;
    }

    if (s_item_count == 0)
    {
        s_empty_lbl = lv_label_create(s_list);
        lv_label_set_text(s_empty_lbl, "No activities found.");
        ui_theme_apply_label(s_empty_lbl, true);
        lv_obj_center(s_empty_lbl);
        update_nav_controls();
        return;
    }

    size_t start = s_page_index * PAGE_ROWS;
    size_t end = start + PAGE_ROWS;
    if (end > s_item_count)
        end = s_item_count;

    for (size_t i = start; i < end; i++)
    {
        activity_meta_t *it = &s_items[i];

        lv_obj_t *card = lv_btn_create(s_list);
        if (!card)
        {
            ESP_LOGE(TAG, "Failed to allocate activity card");
            break;
        }
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, 66);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE); // IMPORTANT (LVGL v9 default)
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        ui_theme_apply_surface(card);

        lv_obj_add_event_cb(card, activity_card_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        // Distance (top-left)
        char dist[24];
        fmt_dist(dist, sizeof(dist), it->distance_m);
        lv_obj_t *dist_lbl = lv_label_create(card);
        lv_label_set_text(dist_lbl, dist);
        ui_theme_apply_label(dist_lbl, false);
        lv_obj_align(dist_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        // Duration (top-right)
        char dur[24];
        fmt_time_hms(dur, sizeof(dur), it->duration_s);
        lv_obj_t *dur_lbl = lv_label_create(card);
        lv_label_set_text(dur_lbl, dur);
        ui_theme_apply_label(dur_lbl, true);
        lv_obj_align(dur_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        // Date/time (bottom-left)
        struct tm tmv;
        localtime_r(&it->start_ts, &tmv);
        char date[32];
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tmv);

        lv_obj_t *date_lbl = lv_label_create(card);
        lv_label_set_text(date_lbl, date);
        ui_theme_apply_label(date_lbl, true);
        lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        // Safety: labels should never be scrollable/clickable
        lv_obj_clear_flag(dist_lbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dur_lbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(date_lbl, LV_OBJ_FLAG_SCROLLABLE);
        ui_yield_for_idle();
    }

    update_nav_controls();
}

void activity_summary_page_refresh(void)
{
    ui_yield_for_idle();
    bool ok = load_index_file();
    if (!ok)
        s_item_count = 0;
    s_page_index = 0;
    ui_yield_for_idle();
    build_list_ui();
}

void activity_summary_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    // Status bar
    ui_status_bar_create(&s_status, s_root);
    ui_status_bar_set_title(&s_status, "Activity", UI_PAGE_MENU);

    // Body
    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 2, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);

    // Nav row
    s_nav_row = lv_obj_create(body);
    lv_obj_set_width(s_nav_row, lv_pct(100));
    lv_obj_set_height(s_nav_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_nav_row, 0, 0);
    lv_obj_set_style_pad_all(s_nav_row, 0, 0);
    lv_obj_set_flex_flow(s_nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_btn_prev = lv_btn_create(s_nav_row);
    ui_theme_apply_button(s_btn_prev);
    lv_obj_add_event_cb(s_btn_prev, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)"prev");
    lv_obj_t *prev_lbl = lv_label_create(s_btn_prev);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_lbl, ui_theme_palette()->accent_text, 0);
    lv_obj_align(prev_lbl, LV_ALIGN_CENTER, 0, 0);

    s_page_lbl = lv_label_create(s_nav_row);
    ui_theme_apply_label(s_page_lbl, true);
    lv_label_set_text(s_page_lbl, "0/0");

    s_btn_next = lv_btn_create(s_nav_row);
    ui_theme_apply_button(s_btn_next);
    lv_obj_add_event_cb(s_btn_next, nav_btn_event_cb, LV_EVENT_CLICKED, (void *)"next");
    lv_obj_t *next_lbl = lv_label_create(s_btn_next);
    lv_label_set_text(next_lbl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_lbl, ui_theme_palette()->accent_text, 0);
    lv_obj_align(next_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_size(s_btn_prev, 40, 32);
    lv_obj_set_size(s_btn_next, 40, 32);

    // List container (paged, non-scrollable)
    s_list = lv_obj_create(body);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    // Non-scrollable body
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    activity_summary_page_refresh();
}

void activity_summary_page_apply_theme(void)
{
    if (!s_root)
        return;
    ui_status_bar_apply_theme(&s_status);
    // Cards are themed at build time via ui_theme_apply_surface/label.
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
}

void activity_summary_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_status);
}
