#include "ui_step_setup_page.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "ui.h"
#include "interval_program.h"
#include "ui_interval_data_page.h"

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_sb;
static lv_obj_t *s_scroll = NULL;

static lv_obj_t *dd_work, *sb_work;
static lv_obj_t *dd_rest, *sb_rest;
static lv_obj_t *sb_rounds, *sb_spm_start, *sb_spm_step;

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
    if (u == INTERVAL_UNIT_TIME) {
        lv_spinbox_set_range(sb, 10, 3600);
        lv_spinbox_set_step(sb, 10);
        lv_spinbox_set_digit_format(sb, 4, 0);
        lv_spinbox_set_value(sb, 60);
    } else if (u == INTERVAL_UNIT_DISTANCE) {
        lv_spinbox_set_range(sb, 50, 10000);
        lv_spinbox_set_step(sb, 50);
        lv_spinbox_set_digit_format(sb, 5, 0);
        lv_spinbox_set_value(sb, 250);
    } else {
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

static lv_obj_t *make_value_row(lv_obj_t *parent, const char *title, bool show_dd,
                                lv_obj_t **out_dd, lv_obj_t **out_sb)
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
    lv_obj_set_style_pad_column(line, 4, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dd = NULL;
    if (show_dd) {
        dd = lv_dropdown_create(line);
        lv_dropdown_set_options(dd, "Time\nDistance\nStrokes");
        lv_dropdown_set_selected(dd, 0);
        ui_theme_apply_field(dd);
        lv_obj_add_event_cb(dd, dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
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

    if (out_dd)
        *out_dd = dd;
    if (out_sb)
        *out_sb = sb;
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
    cfg.spm_start = (uint8_t)lv_spinbox_get_value(sb_spm_start);
    if (cfg.spm_start == 0)
        cfg.spm_start = 18;
    cfg.spm_step = (uint8_t)lv_spinbox_get_value(sb_spm_step);

    interval_program_set_config(&cfg);
    interval_program_stop();

    ui_set_step_start_armed(true);
    ui_set_interval_data_visible(true);
    ui_go_to_page(UI_INTERVAL_DATA_PAGE, true);
    interval_data_page_show_start_prompt();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_set_step_start_armed(false);
    ui_set_interval_data_visible(false);
    interval_data_page_hide_start_prompt();
    ui_go_to_page(UI_PAGE_MENU, true);
}

void step_setup_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_create(&s_sb, s_root);
    ui_status_bar_set_title(&s_sb, "Step test", UI_PAGE_MENU);

    s_scroll = lv_obj_create(s_root);
    lv_obj_set_width(s_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_scroll, 1);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_scroll, 4, 0);
    lv_obj_set_style_pad_right(s_scroll, 4, 0);
    lv_obj_set_style_pad_row(s_scroll, 2, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_scroll);
    lv_label_set_text(title, "Step Test");
    ui_theme_apply_label(title, false);

    make_value_row(s_scroll, "Work", true, &dd_work, &sb_work);
    make_value_row(s_scroll, "Rest", true, &dd_rest, &sb_rest);
    configure_spinbox_for_unit(sb_work, INTERVAL_UNIT_TIME);
    configure_spinbox_for_unit(sb_rest, INTERVAL_UNIT_TIME);

    make_value_row(s_scroll, "Pieces", false, NULL, &sb_rounds);
    lv_spinbox_set_range(sb_rounds, 1, 20);
    lv_spinbox_set_step(sb_rounds, 1);
    lv_spinbox_set_digit_format(sb_rounds, 2, 0);
    lv_spinbox_set_value(sb_rounds, 6);

    make_value_row(s_scroll, "Start SPM", false, NULL, &sb_spm_start);
    lv_spinbox_set_range(sb_spm_start, 12, 60);
    lv_spinbox_set_step(sb_spm_start, 1);
    lv_spinbox_set_digit_format(sb_spm_start, 2, 0);
    lv_spinbox_set_value(sb_spm_start, 18);

    make_value_row(s_scroll, "SPM step", false, NULL, &sb_spm_step);
    lv_spinbox_set_range(sb_spm_step, 1, 10);
    lv_spinbox_set_step(sb_spm_step, 1);
    lv_spinbox_set_digit_format(sb_spm_step, 2, 0);
    lv_spinbox_set_value(sb_spm_step, 2);

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

void step_setup_page_apply_theme(void)
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
    if (sb_spm_start)
        ui_theme_apply_field(sb_spm_start);
    if (sb_spm_step)
        ui_theme_apply_field(sb_spm_step);
}

void step_setup_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_sb);
}
