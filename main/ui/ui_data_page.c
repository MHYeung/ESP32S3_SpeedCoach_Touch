// main/ui/ui_data_page.c
#include "ui_data_page.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui_theme.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DATA_SLOT_MAX 3

static lv_obj_t *s_root = NULL;

static lv_obj_t *s_slot_box[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_title[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_value[DATA_SLOT_MAX] = {0};
static lv_obj_t *s_slot_unit[DATA_SLOT_MAX] = {0};

static data_metric_t s_slot_metric[DATA_SLOT_MAX] = {
    DATA_METRIC_TIME,
    DATA_METRIC_STROKE_COUNT,
    DATA_METRIC_SPM,
};

static data_values_t s_values = {0};
static ui_orientation_t s_orient = UI_ORIENT_PORTRAIT_0;

/*
 * LVGL grid APIs expect `const int32_t[]` descriptors.
 * Using `lv_coord_t[]` (often int16) breaks the layout because LVGL reads 32-bit values.
 */
static int32_t s_col_land[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static int32_t s_row_land[] = {LV_GRID_FR(4), LV_GRID_FR(3), LV_GRID_TEMPLATE_LAST};

static int32_t s_col_port[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static int32_t s_row_port[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
extern const lv_font_t lv_font_montserrat_20;
#define DATA_FONT_TITLE (&lv_font_montserrat_20)
#elif defined(LV_FONT_MONTSERRAT_16) && LV_FONT_MONTSERRAT_16
extern const lv_font_t lv_font_montserrat_16;
#define DATA_FONT_TITLE (&lv_font_montserrat_16)
#else
#define DATA_FONT_TITLE (LV_FONT_DEFAULT)
#endif

#if defined(LV_FONT_MONTSERRAT_40) && LV_FONT_MONTSERRAT_40
extern const lv_font_t lv_font_montserrat_40;
#define DATA_FONT_VALUE (&lv_font_montserrat_40)
#elif defined(LV_FONT_MONTSERRAT_36) && LV_FONT_MONTSERRAT_36
extern const lv_font_t lv_font_montserrat_36;
#define DATA_FONT_VALUE (&lv_font_montserrat_36)
#elif defined(LV_FONT_MONTSERRAT_28) && LV_FONT_MONTSERRAT_28
extern const lv_font_t lv_font_montserrat_28;
#define DATA_FONT_VALUE (&lv_font_montserrat_28)
#else
#define DATA_FONT_VALUE (LV_FONT_DEFAULT)
#endif

#if defined(LV_FONT_MONTSERRAT_32) && LV_FONT_MONTSERRAT_32
extern const lv_font_t lv_font_montserrat_32;
#define DATA_FONT_VALUE_SMALL (&lv_font_montserrat_32)
#elif defined(LV_FONT_MONTSERRAT_28) && LV_FONT_MONTSERRAT_28
extern const lv_font_t lv_font_montserrat_28;
#define DATA_FONT_VALUE_SMALL (&lv_font_montserrat_28)
#else
#define DATA_FONT_VALUE_SMALL (DATA_FONT_VALUE)
#endif

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
extern const lv_font_t lv_font_montserrat_20;
#define DATA_FONT_UNIT (&lv_font_montserrat_20)
#elif defined(LV_FONT_MONTSERRAT_16) && LV_FONT_MONTSERRAT_16
extern const lv_font_t lv_font_montserrat_16;
#define DATA_FONT_UNIT (&lv_font_montserrat_16)
#else
#define DATA_FONT_UNIT (LV_FONT_DEFAULT)
#endif

static bool is_landscape(ui_orientation_t o)
{
    return (o == UI_ORIENT_LANDSCAPE_90 || o == UI_ORIENT_LANDSCAPE_270);
}

static int slot_count_for_current_orient(void)
{
    return is_landscape(s_orient) ? 3 : 3;
}

static void fmt_time_s(float sec, char *out, size_t out_len)
{
    if (!isfinite(sec) || sec < 0.0f) {
        snprintf(out, out_len, "--:--.-");
        return;
    }

    int total = (int)sec;
    int tenths = (int)lroundf((sec - (float)total) * 10.0f);
    if (tenths >= 10) {
        tenths = 0;
        total += 1;
    }

    int s = total % 60;
    int m = (total / 60) % 60;
    int h = total / 3600;

    if (h > 0) {
        snprintf(out, out_len, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, out_len, "%02d:%02d.%d", m, s, tenths);
    }
}

static void fmt_pace_s_per_500m(float sec, char *out, size_t out_len)
{
    if (!isfinite(sec) || sec <= 0.0f) {
        snprintf(out, out_len, "--:--.-");
        return;
    }
    fmt_time_s(sec, out, out_len);
}

static void fmt_distance_m(float m, char *value_out, size_t value_len, const char **unit_out)
{
    if (!isfinite(m) || m < 0.0f) {
        snprintf(value_out, value_len, "--");
        *unit_out = "m";
        return;
    }

    if (m >= 1000.0f) {
        snprintf(value_out, value_len, "%.2f", (double)(m / 1000.0f));
        *unit_out = "km";
    } else {
        snprintf(value_out, value_len, "%.0f", (double)m);
        *unit_out = "m";
    }
}

static void fmt_seconds(float sec, char *out, size_t out_len)
{
    if (!isfinite(sec) || sec < 0.0f) {
        snprintf(out, out_len, "--");
        return;
    }
    snprintf(out, out_len, "%.2f", (double)sec);
}

static void metric_title_unit(data_metric_t metric, const char **title, const char **unit)
{
    switch (metric) {
    case DATA_METRIC_PACE:
        *title = "Pace";
        *unit = "/500m";
        break;
    case DATA_METRIC_TIME:
        *title = "Time";
        *unit = "";
        break;
    case DATA_METRIC_DISTANCE:
        *title = "Distance";
        *unit = "m";
        break;
    case DATA_METRIC_SPEED:
        *title = "Speed";
        *unit = "km/h";
        break;
    case DATA_METRIC_SPM:
        *title = "SPM";
        *unit = "";
        break;
    case DATA_METRIC_POWER:
        *title = "Power";
        *unit = "W";
        break;
    case DATA_METRIC_STROKE_COUNT:
        *title = "Strokes";
        *unit = "";
        break;
    default:
        *title = "?";
        *unit = "";
        break;
    }
}

static void apply_metric_to_slot(int idx)
{
    if (idx < 0 || idx >= DATA_SLOT_MAX) return;
    if (!s_slot_title[idx] || !s_slot_value[idx] || !s_slot_unit[idx]) return;

    data_metric_t metric = s_slot_metric[idx];

    const char *title = "?";
    const char *unit = "";
    metric_title_unit(metric, &title, &unit);

    char value_buf[32] = {0};
    const char *unit_override = NULL;

    switch (metric) {
    case DATA_METRIC_PACE:
        fmt_pace_s_per_500m(s_values.pace_s_per_500m, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_TIME:
        fmt_time_s(s_values.time_s, value_buf, sizeof(value_buf));
        break;
    case DATA_METRIC_DISTANCE:
        fmt_distance_m(s_values.distance_m, value_buf, sizeof(value_buf), &unit_override);
        break;
    case DATA_METRIC_SPEED:
        if (!isfinite(s_values.speed_mps) || s_values.speed_mps < 0.0f) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            float kmh = s_values.speed_mps * 3.6f;
            snprintf(value_buf, sizeof(value_buf), "%.1f", (double)kmh);
        }
        break;
    case DATA_METRIC_SPM:
        if (!isfinite(s_values.spm) || s_values.spm < 0.0f) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            float spm_int = roundf(s_values.spm);
            snprintf(value_buf, sizeof(value_buf), "%.0f", (double)spm_int);
        }
        break;
    case DATA_METRIC_POWER:
        if (!isfinite(s_values.power_w) || s_values.power_w < 0.0f) {
            snprintf(value_buf, sizeof(value_buf), "--");
        } else {
            snprintf(value_buf, sizeof(value_buf), "%.0f", (double)s_values.power_w);
        }
        break;
    case DATA_METRIC_STROKE_COUNT:
        snprintf(value_buf, sizeof(value_buf), "%lu", (unsigned long)s_values.stroke_count);
        break;
    default:
        snprintf(value_buf, sizeof(value_buf), "--");
        break;
    }

    lv_label_set_text(s_slot_title[idx], title);
    lv_label_set_text(s_slot_value[idx], value_buf);
    lv_label_set_text(s_slot_unit[idx], unit_override ? unit_override : unit);
}

static void slot_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_CLICKED) {
        return;
    }

    uintptr_t idx_u = (uintptr_t)lv_event_get_user_data(e);
    int idx = (int)idx_u;
    if (idx < 0 || idx >= DATA_SLOT_MAX) return;

    s_slot_metric[idx] = (data_metric_t)((s_slot_metric[idx] + 1) % DATA_METRIC_COUNT);
    apply_metric_to_slot(idx);
}

static void style_box(lv_obj_t *box)
{
    ui_theme_apply_surface_border(box);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);
}

static void build_slot(int idx)
{
    s_slot_box[idx] = lv_obj_create(s_root);
    style_box(s_slot_box[idx]);

    lv_obj_add_event_cb(s_slot_box[idx], slot_event_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)idx);
    lv_obj_add_event_cb(s_slot_box[idx], slot_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)idx);

    s_slot_title[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_title[idx], DATA_FONT_TITLE, 0);
    ui_theme_apply_label(s_slot_title[idx], true);
    lv_obj_align(s_slot_title[idx], LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(s_slot_title[idx], "?");

    s_slot_value[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_value[idx], DATA_FONT_VALUE, 0);
    ui_theme_apply_label(s_slot_value[idx], false);
    lv_obj_align(s_slot_value[idx], LV_ALIGN_CENTER, 0, 6);
    lv_label_set_text(s_slot_value[idx], "--");

    s_slot_unit[idx] = lv_label_create(s_slot_box[idx]);
    lv_obj_set_style_text_font(s_slot_unit[idx], DATA_FONT_UNIT, 0);
    ui_theme_apply_label(s_slot_unit[idx], true);
    lv_obj_align(s_slot_unit[idx], LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(s_slot_unit[idx], "");
}

static void apply_fonts_for_orientation(void)
{
    if (!s_slot_value[0] || !s_slot_value[1] || !s_slot_value[2]) return;

    lv_obj_set_style_text_font(s_slot_value[0], DATA_FONT_VALUE, 0);

    const lv_font_t *bottom = is_landscape(s_orient) ? DATA_FONT_VALUE_SMALL : DATA_FONT_VALUE;
    lv_obj_set_style_text_font(s_slot_value[1], bottom, 0);
    lv_obj_set_style_text_font(s_slot_value[2], bottom, 0);
}

static void apply_layout(void)
{
    bool land = is_landscape(s_orient);
    apply_fonts_for_orientation();

    lv_obj_set_layout(s_root, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 0, 0);
    lv_obj_set_style_pad_column(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);

    lv_obj_set_grid_dsc_array(s_root, land ? s_col_land : s_col_port, land ? s_row_land : s_row_port);

    /* Top slot spans full width */
    lv_obj_set_grid_cell(s_slot_box[0],
                         LV_GRID_ALIGN_STRETCH, 0, land ? 2 : 1,
                         LV_GRID_ALIGN_STRETCH, 0, 1);

    if (land) {
        lv_obj_clear_flag(s_slot_box[2], LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_grid_cell(s_slot_box[1],
                             LV_GRID_ALIGN_STRETCH, 0, 1,
                             LV_GRID_ALIGN_STRETCH, 1, 1);

        lv_obj_set_grid_cell(s_slot_box[2],
                             LV_GRID_ALIGN_STRETCH, 1, 1,
                             LV_GRID_ALIGN_STRETCH, 1, 1);
    } else {
        lv_obj_clear_flag(s_slot_box[2], LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_grid_cell(s_slot_box[1],
                             LV_GRID_ALIGN_STRETCH, 0, 1,
                             LV_GRID_ALIGN_STRETCH, 1, 1);
        lv_obj_set_grid_cell(s_slot_box[2],
                             LV_GRID_ALIGN_STRETCH, 0, 1,
                             LV_GRID_ALIGN_STRETCH, 2, 1);                     
    }
}

void data_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_root, LV_DIR_NONE);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);

    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        build_slot(i);
    }

    s_orient = UI_ORIENT_PORTRAIT_0;
    apply_layout();

    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }
}

void data_page_apply_theme(void)
{
    if (!s_root) return;

    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);

    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        if (s_slot_box[i]) style_box(s_slot_box[i]);
        if (s_slot_title[i]) ui_theme_apply_label(s_slot_title[i], true);
        if (s_slot_value[i]) ui_theme_apply_label(s_slot_value[i], false);
        if (s_slot_unit[i]) ui_theme_apply_label(s_slot_unit[i], true);
    }
}

void data_page_set_orientation(ui_orientation_t o)
{
    s_orient = o;

    lvgl_port_lock(0);
    if (s_root) {
        apply_layout();
    }
    lvgl_port_unlock();
}

void data_page_set_metrics(const data_metric_t metrics[], size_t count)
{
    if (!metrics || count == 0) return;

    size_t n = count;
    if (n > DATA_SLOT_MAX) n = DATA_SLOT_MAX;

    lvgl_port_lock(0);

    for (size_t i = 0; i < n; i++) {
        if (metrics[i] < DATA_METRIC_COUNT) {
            s_slot_metric[i] = metrics[i];
        }
    }

    for (int i = 0; i < DATA_SLOT_MAX; i++) {
        apply_metric_to_slot(i);
    }

    lvgl_port_unlock();
}

void data_page_set_values(const data_values_t *v)
{
    if (!v) return;

    s_values = *v;

    lvgl_port_lock(0);

    int slots = slot_count_for_current_orient();
    for (int i = 0; i < slots; i++) {
        apply_metric_to_slot(i);
    }

    lvgl_port_unlock();
}
