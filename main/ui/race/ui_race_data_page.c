#include "ui_race_data_page.h"
#include "ui_theme.h"
#include "ui.h"
#include "ui_format.h"
#include "ui_typography.h"
#include "race_program.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

typedef enum {
    RACE_TINT_EVEN = 0,
    RACE_TINT_AHEAD,
    RACE_TINT_BEHIND,
} race_tint_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_delta_box = NULL;
static lv_obj_t *s_delta_title = NULL;
static lv_obj_t *s_delta_val = NULL;
static lv_obj_t *s_rem_title = NULL;
static lv_obj_t *s_rem_val = NULL;
static lv_obj_t *s_pace_title = NULL;
static lv_obj_t *s_pace_val = NULL;

static lv_obj_t *s_prompt_overlay = NULL;
static bool s_prompt_allowed = false;
static race_tint_t s_last_tint = RACE_TINT_EVEN;

static int32_t s_col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static int32_t s_row_dsc[] = {LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};

static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text)
        return;
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0)
        lv_label_set_text(label, text);
}

static void style_box(lv_obj_t *box)
{
    ui_theme_apply_surface(box);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
}

static void apply_delta_tint(race_tint_t tint)
{
    if (!s_delta_box || tint == s_last_tint)
        return;
    s_last_tint = tint;

    if (tint == RACE_TINT_EVEN) {
        style_box(s_delta_box);
        if (s_delta_val) {
            ui_theme_apply_label(s_delta_val, false);
            lv_obj_set_style_text_font(s_delta_val, ui_font_value_lg(), 0);
        }
        if (s_delta_title)
            ui_theme_apply_label(s_delta_title, true);
        return;
    }

    lv_color_t bg = (tint == RACE_TINT_AHEAD) ? ui_theme_color_rest() : ui_theme_color_alert();
    lv_obj_set_style_bg_color(s_delta_box, bg, 0);
    lv_obj_set_style_bg_opa(s_delta_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_delta_box, 0, 0);
    if (s_delta_val)
        lv_obj_set_style_text_color(s_delta_val, lv_color_white(), 0);
    if (s_delta_title)
        lv_obj_set_style_text_color(s_delta_title, lv_color_white(), 0);
}

static lv_obj_t *create_slot(lv_obj_t *parent, const char *title, lv_obj_t **out_title,
                             lv_obj_t **out_value, bool primary)
{
    lv_obj_t *box = lv_obj_create(parent);
    style_box(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box, 4, 0);

    lv_obj_t *t = lv_label_create(box);
    lv_obj_set_style_text_font(t, ui_font_caption(), 0);
    ui_theme_apply_label(t, true);
    lv_label_set_text(t, title);

    lv_obj_t *v = lv_label_create(box);
    lv_obj_set_style_text_font(v, primary ? ui_font_value_lg() : ui_font_value_sm(), 0);
    ui_theme_apply_label(v, false);
    lv_label_set_text(v, "--");

    if (out_title)
        *out_title = t;
    if (out_value)
        *out_value = v;
    return box;
}

static void apply_layout(void)
{
    if (!s_root || !s_delta_box)
        return;
    lv_obj_set_layout(s_root, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 0, 0);
    lv_obj_set_style_pad_column(s_root, 0, 0);
    lv_obj_set_grid_dsc_array(s_root, s_col_dsc, s_row_dsc);
    lv_obj_set_grid_cell(s_delta_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_rem_val), LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_pace_val), LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
}

static void start_prompt_show_async(void *p)
{
    (void)p;
    if (!s_root || !s_prompt_allowed)
        return;

    race_config_t cfg = {0};
    race_program_get_config(&cfg);

    if (!s_prompt_overlay) {
        lv_obj_t *top = lv_layer_top();
        s_prompt_overlay = lv_obj_create(top);
        lv_obj_set_size(s_prompt_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(s_prompt_overlay, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(s_prompt_overlay, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_prompt_overlay, 0, 0);
        lv_obj_set_flex_flow(s_prompt_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_prompt_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *panel = lv_obj_create(s_prompt_overlay);
        ui_theme_apply_surface(panel);
        lv_obj_set_width(panel, lv_pct(90));
        lv_obj_set_height(panel, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(panel, 14, 0);
        lv_obj_set_style_pad_row(panel, 8, 0);

        lv_obj_t *title = lv_label_create(panel);
        ui_theme_apply_label(title, false);
        lv_label_set_text(title, "Race Ready");

        lv_obj_t *hint = lv_label_create(panel);
        ui_theme_apply_label(hint, true);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_text(hint, "Press PWR key to start");
    }

    lv_obj_t *panel = lv_obj_get_child(s_prompt_overlay, 0);
    if (panel) {
        lv_obj_t *info = lv_obj_get_child(panel, 1);
        if (info) {
            char pace[16];
            ui_fmt_pace_s(cfg.target_pace_s_500, pace, sizeof(pace));
            lv_label_set_text_fmt(info, "%lum  @  %s /500m\nPress PWR key to start",
                                  (unsigned long)cfg.distance_m, pace);
        }
    }
    lv_obj_clear_flag(s_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void start_prompt_hide_async(void *p)
{
    (void)p;
    if (s_prompt_overlay)
        lv_obj_add_flag(s_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
}

void race_data_page_apply_snapshot(const coach_ui_snapshot_t *snap)
{
    if (!s_root || !snap)
        return;

    if (!snap->race_active && !snap->race_finished) {
        label_set_text_if_changed(s_delta_val, "READY");
        label_set_text_if_changed(s_delta_title, "vs target");
        label_set_text_if_changed(s_rem_val, "--");
        label_set_text_if_changed(s_pace_val, "--:--.-");
        apply_delta_tint(RACE_TINT_EVEN);
        return;
    }

    if (snap->race_finished) {
        label_set_text_if_changed(s_delta_val, "FINISH");
        apply_delta_tint(RACE_TINT_AHEAD);
    } else {
        char buf[16];
        float d = snap->race_delta_s;
        if (!isfinite(d)) {
            snprintf(buf, sizeof(buf), "--");
        } else {
            /* ASCII '+' / '-' (0x2B / 0x2D); avoid Unicode minus / plusminus. */
            snprintf(buf, sizeof(buf), "%c%.1f", (d >= 0.0f) ? '+' : '-', (double)fabsf(d));
        }
        label_set_text_if_changed(s_delta_val, buf);
        race_tint_t tint = RACE_TINT_EVEN;
        if (isfinite(snap->race_delta_s)) {
            if (snap->race_delta_s < -1.0f)
                tint = RACE_TINT_AHEAD;
            else if (snap->race_delta_s > 1.0f)
                tint = RACE_TINT_BEHIND;
        }
        apply_delta_tint(tint);
    }

    const char *unit = "m";
    char rem[16];
    ui_fmt_distance_m(snap->race_remaining_m, rem, sizeof(rem), &unit);
    label_set_text_if_changed(s_rem_val, rem);

    char proj[24];
    if (isfinite(snap->race_projected_s)) {
        char tbuf[16];
        ui_fmt_time_s(snap->race_projected_s, tbuf, sizeof(tbuf));
        snprintf(proj, sizeof(proj), "proj %s", tbuf);
    } else {
        snprintf(proj, sizeof(proj), "Remaining");
    }
    label_set_text_if_changed(s_rem_title, proj);

    char pace[16];
    ui_fmt_pace_s(snap->pace_s_per_500m, pace, sizeof(pace));
    label_set_text_if_changed(s_pace_val, pace);
}

void race_data_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_delta_box = create_slot(s_root, "vs target", &s_delta_title, &s_delta_val, true);
    create_slot(s_root, "Remaining", &s_rem_title, &s_rem_val, false);
    create_slot(s_root, "Pace", &s_pace_title, &s_pace_val, false);

    apply_layout();
}

void race_data_page_apply_theme(void)
{
    if (!s_root)
        return;
    s_last_tint = (race_tint_t)(RACE_TINT_BEHIND + 1);
    if (s_delta_box)
        style_box(s_delta_box);
    if (s_delta_title)
        ui_theme_apply_label(s_delta_title, true);
    if (s_delta_val) {
        ui_theme_apply_label(s_delta_val, false);
        lv_obj_set_style_text_font(s_delta_val, &lv_font_montserrat_48, 0);
    }
    if (s_rem_title)
        ui_theme_apply_label(s_rem_title, true);
    if (s_rem_val) {
        style_box(lv_obj_get_parent(s_rem_val));
        ui_theme_apply_label(s_rem_val, false);
    }
    if (s_pace_title)
        ui_theme_apply_label(s_pace_title, true);
    if (s_pace_val) {
        style_box(lv_obj_get_parent(s_pace_val));
        ui_theme_apply_label(s_pace_val, false);
    }
}

void race_data_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    apply_layout();
}

void race_data_page_show_start_prompt(void)
{
    s_prompt_allowed = true;
    lv_async_call(start_prompt_show_async, NULL);
}

void race_data_page_hide_start_prompt(void)
{
    s_prompt_allowed = false;
    lv_async_call(start_prompt_hide_async, NULL);
}
