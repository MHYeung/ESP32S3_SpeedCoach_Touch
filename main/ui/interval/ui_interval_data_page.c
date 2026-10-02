#include "ui_interval_data_page.h"
#include "ui_theme.h"
#include "interval_program.h"
#include "ui.h"
#include "ui_format.h"
#include "ui_typography.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "lvgl.h"

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_live = NULL;
static lv_obj_t *s_rem_box = NULL;
static lv_obj_t *s_live_rem_val = NULL;
static lv_obj_t *s_live_round_val = NULL;
static lv_obj_t *s_live_pace_val = NULL;
static lv_obj_t *s_live_rem_title = NULL;
static lv_obj_t *s_live_round_title = NULL;
static lv_obj_t *s_live_pace_title = NULL;
static lv_obj_t *s_live_spm_title = NULL;
static lv_obj_t *s_live_spm_val = NULL;
static lv_obj_t *s_spm_box = NULL;
static int s_spm_tint = -1;
static lv_obj_t *s_cue_lbl = NULL;

static lv_obj_t *s_prompt_overlay = NULL;
static lv_obj_t *s_prompt_panel = NULL;
static lv_obj_t *s_prompt_rows = NULL;
static lv_obj_t *s_prompt_hint = NULL;
static bool s_prompt_allowed = false;

static lv_obj_t *s_complete_overlay = NULL;
static lv_obj_t *s_complete_panel = NULL;
static lv_obj_t *s_complete_lbl = NULL;
static uint32_t s_complete_hide_at_ms = 0;

static interval_phase_t s_last_phase = INTERVAL_PHASE_IDLE;

static void start_prompt_show_async(void *p);
static void start_prompt_hide_async(void *p);
static void complete_prompt_show_async(void *p);

static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void complete_overlay_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_complete_overlay) {
        lv_obj_add_flag(s_complete_overlay, LV_OBJ_FLAG_HIDDEN);
        s_complete_hide_at_ms = 0;
    }
}

static const char *unit_suffix(interval_unit_t u)
{
    if (u == INTERVAL_UNIT_DISTANCE) {
        return "m";
    }
    if (u == INTERVAL_UNIT_STROKES) {
        return "st";
    }
    return NULL;
}

static void fmt_val(char *out, size_t n, interval_unit_t u, uint32_t v)
{
    if (u == INTERVAL_UNIT_TIME) {
        uint32_t mm = v / 60;
        uint32_t ss = v % 60;
        snprintf(out, n, "%lu:%02lu", (unsigned long)mm, (unsigned long)ss);
    } else {
        /* Unit stays on the caption. The number font has no letters. */
        snprintf(out, n, "%lu", (unsigned long)v);
    }
}

static void fmt_target(char *out, size_t n, interval_unit_t u, uint32_t v)
{
    if (u == INTERVAL_UNIT_TIME) {
        uint32_t mm = v / 60;
        uint32_t ss = v % 60;
        snprintf(out, n, "%lu:%02lu", (unsigned long)mm, (unsigned long)ss);
    } else if (u == INTERVAL_UNIT_DISTANCE) {
        snprintf(out, n, "%lum", (unsigned long)v);
    } else {
        snprintf(out, n, "%lu st", (unsigned long)v);
    }
}

static void style_box(lv_obj_t *box)
{
    ui_theme_apply_surface(box);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_NONE);
}

static void apply_spm_tint(uint8_t target, float spm)
{
    if (!s_spm_box) {
        return;
    }
    int tint = 0;
    if (target > 0 && isfinite(spm)) {
        float d = spm - (float)target;
        if (d < -1.0f) {
            tint = -1;
        } else if (d > 1.0f) {
            tint = 1;
        }
    }
    if (tint == s_spm_tint) {
        return;
    }
    s_spm_tint = tint;
    if (tint == 0) {
        style_box(s_spm_box);
        if (s_live_spm_val) {
            ui_theme_apply_label(s_live_spm_val, false);
        }
        return;
    }
    lv_color_t bg = (tint < 0) ? ui_theme_color_alert() : ui_theme_color_rest();
    lv_obj_set_style_bg_color(s_spm_box, bg, 0);
    lv_obj_set_style_bg_opa(s_spm_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_spm_box, 0, 0);
    if (s_live_spm_val) {
        lv_obj_set_style_text_color(s_live_spm_val, lv_color_white(), 0);
    }
}

static void apply_phase_color(interval_phase_t phase, bool countdown)
{
    if (!s_rem_box) {
        return;
    }
    lv_color_t c = ui_theme_palette()->border;
    if (phase == INTERVAL_PHASE_WORK) {
        c = countdown ? ui_theme_color_alert() : ui_theme_color_work();
    } else if (phase == INTERVAL_PHASE_REST) {
        c = countdown ? ui_theme_color_alert() : ui_theme_color_rest();
    }
    lv_obj_set_style_border_color(s_rem_box, c, 0);
    lv_obj_set_style_border_width(s_rem_box, 3, 0);
}

static lv_obj_t *create_slot(lv_obj_t *parent, const char *title, lv_obj_t **out_title, lv_obj_t **out_value, bool primary)
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
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *v = lv_label_create(box);
    lv_obj_set_style_text_font(v, primary ? ui_font_value_lg() : ui_font_value_sm(), 0);
    ui_theme_apply_label(v, false);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);

    if (out_title) {
        *out_title = t;
    }
    if (out_value) {
        *out_value = v;
    }
    return box;
}

static void apply_live_layout(void)
{
    if (!s_live || !s_live_rem_val || !s_live_pace_val || !s_live_spm_val) {
        return;
    }

    lv_obj_set_layout(s_live, LV_LAYOUT_GRID);
    lv_obj_set_style_pad_all(s_live, 0, 0);
    lv_obj_set_style_pad_top(s_live, 0, 0);
    lv_obj_set_style_pad_row(s_live, 0, 0);
    lv_obj_set_style_pad_column(s_live, 0, 0);
    lv_obj_set_style_border_width(s_live, 0, 0);

    /* Two rows in every orientation: remaining on top, SPM | pace below. */
    static int32_t s_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t s_rows[] = {LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(s_live, s_cols, s_rows);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_live_rem_val), LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_live_spm_val), LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_grid_cell(lv_obj_get_parent(s_live_pace_val), LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
}

void interval_data_page_apply_snapshot(const coach_ui_snapshot_t *snap)
{
    if (!s_root || !snap) {
        return;
    }

    if (s_complete_overlay && s_complete_hide_at_ms > 0) {
        if ((int32_t)(lv_tick_get() - s_complete_hide_at_ms) >= 0) {
            lv_obj_add_flag(s_complete_overlay, LV_OBJ_FLAG_HIDDEN);
            s_complete_hide_at_ms = 0;
        }
    }

    if (!snap->interval_active && snap->interval_phase == INTERVAL_PHASE_IDLE) {
        interval_config_t cfg = {0};
        interval_program_get_config(&cfg);
        if (s_live_rem_val) {
            label_set_text_if_changed(s_live_rem_val, "READY");
        }
        if (s_live_rem_title) {
            label_set_text_if_changed(s_live_rem_title, "Remaining");
        }
        if (s_live_round_val) {
            char buf[16];
            snprintf(buf, sizeof(buf), "0/%u", cfg.rounds);
            label_set_text_if_changed(s_live_round_val, buf);
        }
        if (s_live_pace_val) {
            label_set_text_if_changed(s_live_pace_val, "--:--.-");
        }
        if (s_live_spm_val) {
            label_set_text_if_changed(s_live_spm_val, "--");
        }
        if (s_cue_lbl) {
            lv_obj_add_flag(s_cue_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        apply_phase_color(INTERVAL_PHASE_IDLE, false);
        apply_spm_tint(0, NAN);
        s_last_phase = INTERVAL_PHASE_IDLE;
        return;
    }

    char rem[24];
    fmt_val(rem, sizeof(rem), snap->interval_unit, snap->interval_remaining);
    if (s_live_rem_val) {
        label_set_text_if_changed(s_live_rem_val, rem);
    }

    const char *ph =
        (snap->interval_phase == INTERVAL_PHASE_WORK) ? "WORK" :
        (snap->interval_phase == INTERVAL_PHASE_REST) ? "REST" :
        (snap->interval_phase == INTERVAL_PHASE_DONE) ? "DONE" : "IDLE";
    if (s_live_rem_title) {
        char title[24];
        const char *unit = unit_suffix(snap->interval_unit);
        if (snap->rounds && unit) {
            snprintf(title, sizeof(title), "%s %u/%u  %s", ph,
                     (unsigned)snap->round_idx, (unsigned)snap->rounds, unit);
        } else if (snap->rounds) {
            snprintf(title, sizeof(title), "%s %u/%u", ph,
                     (unsigned)snap->round_idx, (unsigned)snap->rounds);
        } else if (unit) {
            snprintf(title, sizeof(title), "%s  %s", ph, unit);
        } else {
            snprintf(title, sizeof(title), "%s", ph);
        }
        label_set_text_if_changed(s_live_rem_title, title);
        lv_color_t tc = (snap->interval_phase == INTERVAL_PHASE_WORK) ? ui_theme_color_work() :
                        (snap->interval_phase == INTERVAL_PHASE_REST) ? ui_theme_color_rest() :
                        ui_theme_palette()->text_muted;
        lv_obj_set_style_text_color(s_live_rem_title, tc, 0);
    }

    if (s_live_round_val) {
        if (snap->rounds) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u/%u", snap->round_idx, snap->rounds);
            label_set_text_if_changed(s_live_round_val, buf);
        } else {
            label_set_text_if_changed(s_live_round_val, "--");
        }
    }

    if (s_live_pace_val) {
        char pace[16];
        ui_fmt_pace_s(snap->pace_s_per_500m, pace, sizeof(pace));
        label_set_text_if_changed(s_live_pace_val, pace);
    }
    if (s_live_spm_val) {
        char buf[12];
        ui_fmt_spm(snap->spm, buf, sizeof(buf));
        label_set_text_if_changed(s_live_spm_val, buf);
    }
    if (s_live_spm_title) {
        if (snap->target_spm > 0) {
            char tbuf[16];
            snprintf(tbuf, sizeof(tbuf), "SPM %u", (unsigned)snap->target_spm);
            label_set_text_if_changed(s_live_spm_title, tbuf);
        } else {
            label_set_text_if_changed(s_live_spm_title, "SPM");
        }
    }
    apply_spm_tint(snap->target_spm, snap->spm);

    bool countdown = (snap->interval_unit == INTERVAL_UNIT_TIME &&
                      snap->interval_remaining <= 5 &&
                      (snap->interval_phase == INTERVAL_PHASE_WORK ||
                       snap->interval_phase == INTERVAL_PHASE_REST));
    apply_phase_color(snap->interval_phase, countdown);

    if (s_cue_lbl) {
        if (countdown) {
            const char *next = (snap->interval_phase == INTERVAL_PHASE_WORK) ? "REST" : "WORK";
            char buf[24];
            snprintf(buf, sizeof(buf), "%s in %us", next, (unsigned)snap->interval_remaining);
            label_set_text_if_changed(s_cue_lbl, buf);
            lv_obj_clear_flag(s_cue_lbl, LV_OBJ_FLAG_HIDDEN);
        } else if (snap->interval_phase != s_last_phase &&
                   (snap->interval_phase == INTERVAL_PHASE_WORK || snap->interval_phase == INTERVAL_PHASE_REST)) {
            label_set_text_if_changed(s_cue_lbl, ph);
            lv_obj_clear_flag(s_cue_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cue_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_last_phase = snap->interval_phase;
}

void interval_data_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_live = s_root;

    s_rem_box = create_slot(s_live, "WORK", &s_live_rem_title, &s_live_rem_val, true);
    s_spm_box = create_slot(s_live, "SPM", &s_live_spm_title, &s_live_spm_val, false);
    create_slot(s_live, "Pace", &s_live_pace_title, &s_live_pace_val, false);

    s_cue_lbl = lv_label_create(s_root);
    ui_theme_apply_label(s_cue_lbl, false);
    lv_obj_set_style_text_font(s_cue_lbl, ui_font_caption(), 0);
    lv_obj_align(s_cue_lbl, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_add_flag(s_cue_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_cue_lbl, "");

    apply_live_layout();
}

void interval_data_page_apply_theme(void)
{
    if (!s_root) {
        return;
    }
    if (s_live_rem_title) {
        ui_theme_apply_label(s_live_rem_title, true);
    }
    if (s_live_round_title) {
        ui_theme_apply_label(s_live_round_title, true);
    }
    if (s_live_spm_title) {
        ui_theme_apply_label(s_live_spm_title, true);
    }
    if (s_live_pace_title) {
        ui_theme_apply_label(s_live_pace_title, true);
    }
    if (s_live_rem_val) {
        style_box(lv_obj_get_parent(s_live_rem_val));
        ui_theme_apply_label(s_live_rem_val, false);
    }
    if (s_live_round_val) {
        style_box(lv_obj_get_parent(s_live_round_val));
        ui_theme_apply_label(s_live_round_val, false);
    }
    if (s_live_spm_val) {
        style_box(lv_obj_get_parent(s_live_spm_val));
        ui_theme_apply_label(s_live_spm_val, false);
    }
    if (s_live_pace_val) {
        style_box(lv_obj_get_parent(s_live_pace_val));
        ui_theme_apply_label(s_live_pace_val, false);
    }
    apply_phase_color(s_last_phase, false);
    s_spm_tint = -1;
}

void interval_data_page_on_orientation_changed(void)
{
    if (!s_root) {
        return;
    }
    apply_live_layout();
}

static void start_prompt_show_async(void *p)
{
    (void)p;
    if (!s_root || !s_prompt_allowed) {
        return;
    }

    interval_config_t cfg = {0};
    interval_program_get_config(&cfg);

    if (!s_prompt_overlay) {
        lv_obj_t *top = lv_layer_top();
        s_prompt_overlay = lv_obj_create(top);
        lv_obj_set_size(s_prompt_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(s_prompt_overlay, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(s_prompt_overlay, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_prompt_overlay, 0, 0);
        lv_obj_set_flex_flow(s_prompt_overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_prompt_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_prompt_panel = lv_obj_create(s_prompt_overlay);
        ui_theme_apply_surface(s_prompt_panel);
        lv_obj_set_width(s_prompt_panel, lv_pct(94));
        lv_obj_set_height(s_prompt_panel, lv_pct(85));
        lv_obj_set_style_pad_all(s_prompt_panel, 14, 0);
        lv_obj_set_style_pad_row(s_prompt_panel, 10, 0);
        lv_obj_set_flex_flow(s_prompt_panel, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *title = lv_label_create(s_prompt_panel);
        ui_theme_apply_label(title, false);
        lv_label_set_text(title, "Interval Ready");

        s_prompt_rows = lv_obj_create(s_prompt_panel);
        lv_obj_set_style_bg_opa(s_prompt_rows, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_prompt_rows, 0, 0);
        lv_obj_set_style_pad_all(s_prompt_rows, 0, 0);
        lv_obj_set_flex_flow(s_prompt_rows, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_prompt_rows, 6, 0);
        lv_obj_set_flex_grow(s_prompt_rows, 1);

        s_prompt_hint = lv_label_create(s_prompt_panel);
        ui_theme_apply_label(s_prompt_hint, true);
        lv_label_set_long_mode(s_prompt_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_prompt_hint, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (s_prompt_rows) {
        lv_obj_clean(s_prompt_rows);
        char work[24];
        char rest[24];
        fmt_target(work, sizeof(work), cfg.work.unit, cfg.work.value);
        fmt_target(rest, sizeof(rest), cfg.rest.unit, cfg.rest.value);

        lv_obj_t *r1 = lv_label_create(s_prompt_rows);
        ui_theme_apply_label(r1, true);
        lv_label_set_text_fmt(r1, "Work: %s", work);

        lv_obj_t *r2 = lv_label_create(s_prompt_rows);
        ui_theme_apply_label(r2, true);
        lv_label_set_text_fmt(r2, "Rest: %s", rest);

        lv_obj_t *r3 = lv_label_create(s_prompt_rows);
        ui_theme_apply_label(r3, true);
        lv_label_set_text_fmt(r3, "Rounds: %u", cfg.rounds);

        if (cfg.spm_start > 0) {
            lv_obj_t *r4 = lv_label_create(s_prompt_rows);
            ui_theme_apply_label(r4, true);
            lv_label_set_text_fmt(r4, "SPM: %u +%u/rd", cfg.spm_start, cfg.spm_step);
        }
    }

    if (s_prompt_hint) {
        lv_label_set_text(s_prompt_hint, "Press PWR key to start");
    }
    if (s_prompt_overlay) {
        lv_obj_clear_flag(s_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_prompt_hide_async(void *p)
{
    (void)p;
    if (s_prompt_overlay) {
        lv_obj_add_flag(s_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void complete_prompt_show_async(void *p)
{
    (void)p;
    if (!s_root) {
        return;
    }
    if (!s_complete_overlay) {
        lv_obj_t *top = lv_layer_top();
        s_complete_overlay = lv_obj_create(top);
        lv_obj_set_size(s_complete_overlay, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(s_complete_overlay, LV_OPA_40, 0);
        lv_obj_set_style_bg_color(s_complete_overlay, lv_color_black(), 0);
        lv_obj_set_style_border_width(s_complete_overlay, 0, 0);
        lv_obj_clear_flag(s_complete_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_complete_overlay, complete_overlay_event_cb, LV_EVENT_CLICKED, NULL);

        s_complete_panel = lv_obj_create(s_complete_overlay);
        lv_obj_set_size(s_complete_panel, 180, 70);
        lv_obj_set_style_bg_opa(s_complete_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_complete_panel, ui_theme_color_accent(), 0);
        lv_obj_set_style_border_width(s_complete_panel, 0, 0);
        lv_obj_set_style_radius(s_complete_panel, 6, 0);
        lv_obj_center(s_complete_panel);
        lv_obj_clear_flag(s_complete_panel, LV_OBJ_FLAG_SCROLLABLE);

        s_complete_lbl = lv_label_create(s_complete_panel);
        lv_label_set_text(s_complete_lbl, "Interval complete");
        lv_obj_set_style_text_color(s_complete_lbl, ui_theme_palette()->accent_text, 0);
        lv_obj_center(s_complete_lbl);
    }
    s_complete_hide_at_ms = lv_tick_get() + 5000;
    if (s_complete_overlay) {
        lv_obj_clear_flag(s_complete_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void interval_data_page_show_start_prompt(void)
{
    s_prompt_allowed = true;
    lv_async_call(start_prompt_show_async, NULL);
}

void interval_data_page_hide_start_prompt(void)
{
    s_prompt_allowed = false;
    lv_async_call(start_prompt_hide_async, NULL);
}

void interval_data_page_show_complete_prompt(void)
{
    lv_async_call(complete_prompt_show_async, NULL);
}
