#include "ui_race_setup_page.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "ui.h"
#include "race_program.h"
#include "ui_race_data_page.h"

#include <stdio.h>

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_sb;
static lv_obj_t *s_scroll = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_dd_dist = NULL;
static lv_obj_t *s_sb_custom = NULL;
static lv_obj_t *s_sb_min = NULL;
static lv_obj_t *s_sb_sec = NULL;
static lv_obj_t *s_custom_row = NULL;

static const uint32_t s_dist_preset[] = {200, 500, 1000, 2000};

static void dist_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_dd_dist || !s_custom_row)
        return;
    uint32_t idx = lv_dropdown_get_selected(s_dd_dist);
    if (idx >= 4)
        lv_obj_clear_flag(s_custom_row, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_custom_row, LV_OBJ_FLAG_HIDDEN);
}

static uint32_t selected_distance_m(void)
{
    uint32_t idx = lv_dropdown_get_selected(s_dd_dist);
    if (idx < 4)
        return s_dist_preset[idx];
    int32_t v = lv_spinbox_get_value(s_sb_custom);
    if (v < 50)
        v = 50;
    return (uint32_t)v;
}

static float selected_pace_s(void)
{
    int32_t mm = lv_spinbox_get_value(s_sb_min);
    int32_t ss = lv_spinbox_get_value(s_sb_sec);
    float pace = (float)(mm * 60 + ss);
    if (pace < 30.0f)
        pace = 30.0f;
    return pace;
}

static void confirm_cb(lv_event_t *e)
{
    (void)e;
    race_config_t cfg = {
        .distance_m = selected_distance_m(),
        .target_pace_s_500 = selected_pace_s(),
    };
    race_program_set_config(&cfg);
    race_program_stop();

    ui_set_race_start_armed(true);
    ui_set_race_data_visible(true);
    ui_go_to_page(UI_RACE_DATA_PAGE, true);
    race_data_page_show_start_prompt();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_set_race_start_armed(false);
    ui_set_race_data_visible(false);
    race_data_page_hide_start_prompt();
    ui_go_to_page(UI_PAGE_MENU, true);
}

static lv_obj_t *make_spin_pair(lv_obj_t *parent, const char *title, lv_obj_t **out_a, lv_obj_t **out_b,
                                const char *mid)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    ui_theme_apply_label(lbl, false);

    lv_obj_t *line = lv_obj_create(row);
    lv_obj_set_width(line, lv_pct(100));
    lv_obj_set_height(line, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 6, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *a = lv_spinbox_create(line);
    lv_obj_set_width(a, 72);
    ui_theme_apply_field(a);
    ui_yield_for_idle();
    lv_obj_t *sep = lv_label_create(line);
    lv_label_set_text(sep, mid);
    ui_theme_apply_label(sep, true);
    lv_obj_t *b = lv_spinbox_create(line);
    lv_obj_set_width(b, 72);
    ui_theme_apply_field(b);
    ui_yield_for_idle();

    if (out_a)
        *out_a = a;
    if (out_b)
        *out_b = b;
    return row;
}

void race_setup_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_create(&s_sb, s_root);
    ui_status_bar_set_title(&s_sb, "Race", UI_PAGE_MENU);

    s_scroll = lv_obj_create(s_root);
    lv_obj_set_width(s_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_scroll, 1);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_scroll, 6, 0);
    lv_obj_set_style_pad_right(s_scroll, 6, 0);
    lv_obj_set_style_pad_row(s_scroll, 6, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);

    s_title = lv_label_create(s_scroll);
    lv_label_set_text(s_title, "Race Setup");
    ui_theme_apply_label(s_title, false);

    make_spin_pair(s_scroll, "Target pace /500m", &s_sb_min, &s_sb_sec, ":");
    lv_spinbox_set_range(s_sb_min, 0, 9);
    lv_spinbox_set_step(s_sb_min, 1);
    lv_spinbox_set_digit_format(s_sb_min, 1, 0);
    lv_spinbox_set_value(s_sb_min, 2);
    lv_spinbox_set_range(s_sb_sec, 0, 59);
    lv_spinbox_set_step(s_sb_sec, 1);
    lv_spinbox_set_digit_format(s_sb_sec, 2, 0);
    lv_spinbox_set_value(s_sb_sec, 0);

    lv_obj_t *dist_lbl = lv_label_create(s_scroll);
    lv_label_set_text(dist_lbl, "Race distance");
    ui_theme_apply_label(dist_lbl, false);

    s_dd_dist = lv_dropdown_create(s_scroll);
    lv_dropdown_set_options(s_dd_dist, "200 m\n500 m\n1000 m\n2000 m\nCustom");
    lv_dropdown_set_selected(s_dd_dist, 2);
    lv_obj_set_width(s_dd_dist, lv_pct(100));
    ui_theme_apply_field(s_dd_dist);
    lv_obj_add_event_cb(s_dd_dist, dist_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_custom_row = lv_obj_create(s_scroll);
    lv_obj_set_width(s_custom_row, lv_pct(100));
    lv_obj_set_height(s_custom_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_custom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_custom_row, 0, 0);
    lv_obj_set_style_pad_all(s_custom_row, 0, 0);
    lv_obj_add_flag(s_custom_row, LV_OBJ_FLAG_HIDDEN);

    s_sb_custom = lv_spinbox_create(s_custom_row);
    lv_obj_set_width(s_sb_custom, lv_pct(100));
    ui_theme_apply_field(s_sb_custom);
    ui_yield_for_idle();
    lv_spinbox_set_range(s_sb_custom, 50, 10000);
    lv_spinbox_set_step(s_sb_custom, 50);
    lv_spinbox_set_digit_format(s_sb_custom, 5, 0);
    lv_spinbox_set_value(s_sb_custom, 1500);

    lv_obj_t *btn_row = lv_obj_create(s_scroll);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_cancel = lv_btn_create(btn_row);
    lv_obj_add_event_cb(b_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(b_cancel, lv_pct(48));
    ui_theme_apply_button_secondary(b_cancel);
    lv_obj_t *lc = lv_label_create(b_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_center(lc);

    lv_obj_t *b_confirm = lv_btn_create(btn_row);
    lv_obj_add_event_cb(b_confirm, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(b_confirm, lv_pct(48));
    ui_theme_apply_button(b_confirm);
    lv_obj_t *lq = lv_label_create(b_confirm);
    lv_label_set_text(lq, "Confirm");
    lv_obj_center(lq);
}

void race_setup_page_apply_theme(void)
{
    if (!s_root)
        return;
    ui_status_bar_apply_theme(&s_sb);
    if (s_dd_dist)
        ui_theme_apply_field(s_dd_dist);
    if (s_sb_custom)
        ui_theme_apply_field(s_sb_custom);
    if (s_sb_min)
        ui_theme_apply_field(s_sb_min);
    if (s_sb_sec)
        ui_theme_apply_field(s_sb_sec);
}

void race_setup_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_sb);
}
