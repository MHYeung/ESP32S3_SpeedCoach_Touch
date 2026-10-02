#include "ui_interval_setup_page.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "ui.h"
#include "interval_program.h"
#include "ui_interval_data_page.h"
#include <stdio.h>

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_sb;
static lv_obj_t *s_scroll = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_row_work = NULL;
static lv_obj_t *s_row_rest = NULL;
static lv_obj_t *s_line_work = NULL;
static lv_obj_t *s_line_rest = NULL;
static lv_obj_t *s_line_rounds = NULL;
static lv_obj_t *s_rounds_spacer = NULL;
static lv_obj_t *s_row_rounds = NULL;
static lv_obj_t *s_btn_row = NULL;

static lv_obj_t *dd_work, *sb_work;
static lv_obj_t *dd_rest, *sb_rest;
static lv_obj_t *sb_rounds;

// -----------------------
// Async relayout scheduler
// -----------------------
static bool s_relayout_scheduled = false;
static bool s_relayout_running = false;
static bool s_relayout_pending = false;

static void relayout_now(void);
static void relayout_async_cb(void *user_data);

static void relayout_request(void)
{
    // Coalesce multiple requests
    s_relayout_pending = true;

    if (s_relayout_scheduled)
        return;
    s_relayout_scheduled = true;

    lv_async_call(relayout_async_cb, NULL);
}

static void relayout_async_cb(void *user_data)
{
    (void)user_data;

    // allow future schedules
    s_relayout_scheduled = false;

    // page not ready (or was destroyed)
    if (!s_root)
    {
        s_relayout_pending = false;
        return;
    }

    // prevent re-entrancy
    if (s_relayout_running)
    {
        // if something requested while running, schedule another pass
        if (!s_relayout_scheduled)
        {
            s_relayout_scheduled = true;
            lv_async_call(relayout_async_cb, NULL);
        }
        return;
    }

    s_relayout_running = true;

    // drain pending requests
    while (s_relayout_pending)
    {
        s_relayout_pending = false;
        relayout_now();
    }

    s_relayout_running = false;
}

static interval_unit_t dd_to_unit(uint32_t idx)
{
    if (idx == 0)
        return INTERVAL_UNIT_TIME;
    if (idx == 1)
        return INTERVAL_UNIT_DISTANCE;
    return INTERVAL_UNIT_STROKES;
}

static void configure_spinbox_for_unit(lv_obj_t *sb, interval_unit_t u)
{
    if (u == INTERVAL_UNIT_TIME)
    {
        lv_spinbox_set_range(sb, 10, 3600);
        lv_spinbox_set_step(sb, 10);
        lv_spinbox_set_digit_format(sb, 4, 0);
        lv_spinbox_set_value(sb, 60);
    }
    else if (u == INTERVAL_UNIT_DISTANCE)
    {
        lv_spinbox_set_range(sb, 50, 10000);
        lv_spinbox_set_step(sb, 50);
        lv_spinbox_set_digit_format(sb, 5, 0);
        lv_spinbox_set_value(sb, 500);
    }
    else
    {
        lv_spinbox_set_range(sb, 1, 60);
        lv_spinbox_set_step(sb, 1);
        lv_spinbox_set_digit_format(sb, 2, 0);
        lv_spinbox_set_value(sb, 10);
    }
}

static void dd_changed_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t idx = lv_dropdown_get_selected(dd);

    if (dd == dd_work)
        configure_spinbox_for_unit(sb_work, dd_to_unit(idx));
    if (dd == dd_rest)
        configure_spinbox_for_unit(sb_rest, dd_to_unit(idx));
}

static void btn_inc_cb(lv_event_t *e) { lv_spinbox_increment(lv_event_get_user_data(e)); }
static void btn_dec_cb(lv_event_t *e) { lv_spinbox_decrement(lv_event_get_user_data(e)); }

static lv_obj_t *make_row(lv_obj_t *parent, const char *title, lv_obj_t **out_dd, lv_obj_t **out_sb,
                          lv_obj_t **out_line, bool show_dd, lv_obj_t **out_spacer)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 2, 0);

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
    lv_obj_set_style_pad_column(line, 4, 0);

    lv_obj_t *dd = NULL;
    lv_obj_t *spacer = NULL;
    if (show_dd)
    {
        dd = lv_dropdown_create(line);
        lv_dropdown_set_options(dd, "Time\nDistance\nStrokes");
        lv_dropdown_set_selected(dd, 0);
        ui_theme_apply_field(dd);
        lv_obj_add_event_cb(dd, dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    else
    {
        spacer = lv_obj_create(line);
        lv_obj_set_size(spacer, 0, 1);
        lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(spacer, 0, 0);
        lv_obj_set_style_pad_all(spacer, 0, 0);
    }

    lv_obj_t *dec = lv_btn_create(line);
    ui_theme_apply_stepper(dec);
    lv_obj_t *d = lv_label_create(dec);
    lv_label_set_text(d, "-");
    lv_obj_center(d);

    lv_obj_t *sb = lv_spinbox_create(line);
    lv_obj_set_width(sb, 76);
    ui_theme_apply_field(sb);
    ui_yield_for_idle();

    lv_obj_t *inc = lv_btn_create(line);
    ui_theme_apply_stepper(inc);
    lv_obj_t *i = lv_label_create(inc);
    lv_label_set_text(i, "+");
    lv_obj_center(i);

    lv_obj_add_event_cb(inc, btn_inc_cb, LV_EVENT_CLICKED, sb);
    lv_obj_add_event_cb(dec, btn_dec_cb, LV_EVENT_CLICKED, sb);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (show_dd)
        configure_spinbox_for_unit(sb, INTERVAL_UNIT_TIME);

    if (out_dd)
        *out_dd = dd;
    if (out_sb)
        *out_sb = sb;
    if (out_line)
        *out_line = line;
    if (out_spacer)
        *out_spacer = spacer;
    return row;
}

static void confirm_cb(lv_event_t *e)
{
    (void)e;

    interval_config_t cfg = {0};
    cfg.work.unit = dd_to_unit(lv_dropdown_get_selected(dd_work));
    cfg.work.value = (uint32_t)lv_spinbox_get_value(sb_work);

    cfg.rest.unit = dd_to_unit(lv_dropdown_get_selected(dd_rest));
    cfg.rest.value = (uint32_t)lv_spinbox_get_value(sb_rest);

    cfg.rounds = (uint16_t)lv_spinbox_get_value(sb_rounds);
    if (cfg.rounds == 0)
        cfg.rounds = 1;
    cfg.auto_advance = true;
    cfg.spm_start = 0;
    cfg.spm_step = 0;

    interval_program_set_config(&cfg);
    interval_program_stop(); // IMPORTANT: do NOT start here

    // Go to interval data page (await user press Start there)
    ui_set_interval_start_armed(true);
    ui_set_interval_data_visible(true);
    ui_go_to_page(UI_INTERVAL_DATA_PAGE, true);
    interval_data_page_show_start_prompt();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_set_interval_start_armed(false);
    ui_set_interval_data_visible(false);
    interval_data_page_hide_start_prompt();
    ui_go_to_page(UI_PAGE_MENU, true);
}

static void relayout_now(void)
{
    if (!s_root)
        return;

    bool land = ui_is_landscape();
    lv_coord_t pad_lr = land ? 6 : 2;
    lv_coord_t row_pad = land ? 1 : 0;
    lv_coord_t col_pad = land ? 6 : 4;
    lv_coord_t dd_w = land ? 108 : 82;
    lv_coord_t sb_w = land ? 82 : 64;

    if (s_scroll)
    {
        lv_obj_set_style_pad_left(s_scroll, pad_lr, 0);
        lv_obj_set_style_pad_right(s_scroll, pad_lr, 0);
        lv_obj_set_style_pad_row(s_scroll, land ? 1 : 0, 0);
    }

    if (s_row_work)
        lv_obj_set_style_pad_row(s_row_work, row_pad, 0);
    if (s_row_rest)
        lv_obj_set_style_pad_row(s_row_rest, row_pad, 0);
    if (s_line_work)
        lv_obj_set_style_pad_column(s_line_work, col_pad, 0);
    if (s_line_rest)
        lv_obj_set_style_pad_column(s_line_rest, col_pad, 0);
    if (s_line_rounds)
        lv_obj_set_style_pad_column(s_line_rounds, col_pad, 0);
    if (s_row_rounds)
        lv_obj_set_style_pad_row(s_row_rounds, row_pad, 0);
    if (s_btn_row)
        lv_obj_set_style_pad_row(s_btn_row, row_pad, 0);

    if (dd_work && lv_obj_get_width(dd_work) != dd_w)
        lv_obj_set_width(dd_work, dd_w);
    if (dd_rest && lv_obj_get_width(dd_rest) != dd_w)
        lv_obj_set_width(dd_rest, dd_w);
    if (s_rounds_spacer && lv_obj_get_width(s_rounds_spacer) != dd_w)
        lv_obj_set_width(s_rounds_spacer, dd_w);
    if (sb_work && lv_obj_get_width(sb_work) != dd_w)
        lv_obj_set_width(sb_work, dd_w);
    if (sb_rest && lv_obj_get_width(sb_rest) != dd_w)
        lv_obj_set_width(sb_rest, dd_w);
    if (sb_rounds && lv_obj_get_width(sb_rounds) != dd_w)
        lv_obj_set_width(sb_rounds, dd_w);
}

void interval_setup_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed status bar
    ui_status_bar_create(&s_sb, s_root);
    ui_status_bar_set_title(&s_sb, "Interval", UI_PAGE_MENU);

    // ONE scroll container for everything else
    s_scroll = lv_obj_create(s_root);
    lv_obj_set_width(s_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_scroll, 1); // take remaining height under status bar
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_scroll, 2, 0);
    lv_obj_set_style_pad_right(s_scroll, 2, 0);
    lv_obj_set_style_pad_row(s_scroll, 1, 0);

    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scroll, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Title inside scroll (so everything is one column)
    s_title = lv_label_create(s_scroll);
    lv_label_set_text(s_title, "Interval Setup");
    ui_theme_apply_label(s_title, false);
    lv_obj_set_height(s_title, LV_SIZE_CONTENT);

    // Work / Rest rows
    s_row_work = make_row(s_scroll, "Work", &dd_work, &sb_work, &s_line_work, true, NULL);
    s_row_rest = make_row(s_scroll, "Rest", &dd_rest, &sb_rest, &s_line_rest, true, NULL);
    lv_obj_set_style_text_align(sb_work, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_align(sb_rest, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Rounds row
    s_row_rounds = make_row(s_scroll, "Rounds", NULL, &sb_rounds, &s_line_rounds, false, &s_rounds_spacer);
    lv_spinbox_set_range(sb_rounds, 1, 30);
    lv_spinbox_set_step(sb_rounds, 1);
    lv_spinbox_set_digit_format(sb_rounds, 2, 0);
    lv_spinbox_set_value(sb_rounds, 10);
    lv_obj_set_style_text_align(sb_rounds, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // Buttons row INSIDE scroll (so they scroll with the column)
    s_btn_row = lv_obj_create(s_scroll);
    lv_obj_set_width(s_btn_row, lv_pct(100));
    lv_obj_set_height(s_btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_row, 0, 0);
    lv_obj_set_style_pad_all(s_btn_row, 0, 0);
    lv_obj_set_flex_flow(s_btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_cancel = lv_btn_create(s_btn_row);
    lv_obj_add_event_cb(b_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(b_cancel, lv_pct(48));
    ui_theme_apply_button_secondary(b_cancel);
    lv_obj_t *lc = lv_label_create(b_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_center(lc);

    lv_obj_t *b_confirm = lv_btn_create(s_btn_row);
    lv_obj_add_event_cb(b_confirm, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(b_confirm, lv_pct(48));
    ui_theme_apply_button(b_confirm);
    lv_obj_t *lq = lv_label_create(b_confirm);
    lv_label_set_text(lq, "Confirm");
    lv_obj_center(lq);

    /* Run initial relayout synchronously to avoid async timing issues during first show */
    relayout_now();
}

void interval_setup_page_apply_theme(void)
{
    if (!s_root)
        return;
    ui_status_bar_apply_theme(&s_sb);
    if (dd_work)
        ui_theme_apply_field(dd_work);
    if (dd_rest)
        ui_theme_apply_field(dd_rest);
    if (sb_work)
        ui_theme_apply_field(sb_work);
    if (sb_rest)
        ui_theme_apply_field(sb_rest);
    if (sb_rounds)
        ui_theme_apply_field(sb_rounds);
}
void interval_setup_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_sb);
    relayout_request();
}
