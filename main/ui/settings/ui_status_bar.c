#include "ui/settings/ui_status_bar.h"
#include "ui/settings/ui_theme.h"
#include "ui/core/ui_typography.h"
#include "battery_drv.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_status_bar";

#define UI_STATUS_BAR_MAX 8

static ui_status_bar_t *s_bars[UI_STATUS_BAR_MAX];
static uint8_t s_bar_count = 0;
static ui_status_bar_t *s_default_bar = NULL;

static bool s_cached_gps_connected = false;
static uint8_t s_cached_gps_bars = 0;
static bool s_cached_gps_stale = false;
static int s_cached_batt_pct = -1;

static battery_drv_handle_t s_bat = NULL;
static bool s_bat_inited = false;
static lv_timer_t *s_shared_clock = NULL;
static lv_timer_t *s_shared_batt = NULL;

static int32_t s_cols_compact[] = {LV_GRID_FR(2), LV_GRID_FR(2), LV_GRID_CONTENT, LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
static int32_t s_rows[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

static bool status_bar_is_landscape(void)
{
    lv_display_t *disp = lv_disp_get_default();
    lv_display_rotation_t r = lv_display_get_rotation(disp);
    return (r == LV_DISPLAY_ROTATION_90 || r == LV_DISPLAY_ROTATION_270);
}

static void register_bar(ui_status_bar_t *bar)
{
    if (!bar) {
        return;
    }
    for (uint8_t i = 0; i < s_bar_count; i++) {
        if (s_bars[i] == bar) {
            return;
        }
    }
    if (s_bar_count < UI_STATUS_BAR_MAX) {
        s_bars[s_bar_count++] = bar;
    }
}

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

void ui_status_bar_set_default(ui_status_bar_t *bar)
{
    s_default_bar = bar;
}

static void status_bar_set_time_placeholder(ui_status_bar_t *bar)
{
    if (!bar || !bar->time_label) {
        return;
    }
    label_set_text_if_changed(bar->time_label, status_bar_is_landscape() ? "--:--:--" : "--:--");
}

static void title_back_cb(lv_event_t *e)
{
    ui_status_bar_t *bar = (ui_status_bar_t *)lv_event_get_user_data(e);
    if (!bar || !bar->has_back) {
        return;
    }
    ui_go_to_page(bar->back_page, true);
}

static void status_bar_apply_layout(ui_status_bar_t *bar)
{
    bool land = status_bar_is_landscape();
    if (bar->kind == UI_STATUS_BAR_COMPACT) {
        lv_obj_set_grid_dsc_array(bar->root, s_cols_compact, s_rows);
        lv_obj_set_style_pad_hor(bar->root, 6, 0);
        lv_obj_set_style_pad_ver(bar->root, 1, 0);
        lv_obj_set_height(bar->root, UI_STATUS_BAR_COMPACT_H);
        return;
    }
    (void)land;
    lv_obj_set_layout(bar->root, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(bar->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar->root, 0, 0);
    lv_obj_set_style_radius(bar->root, 0, 0);
    lv_obj_set_style_pad_hor(bar->root, 2, 0);
    lv_obj_set_style_pad_ver(bar->root, 0, 0);
    lv_obj_set_height(bar->root, UI_STATUS_BAR_FULL_H);
}

static void status_bar_battery_init_once(void)
{
    if (s_bat_inited) {
        return;
    }
    s_bat_inited = true;

    battery_drv_config_t cfg = {
        .unit = ADC_UNIT_1,
        .channel = ADC_CHANNEL_7,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .divider_ratio = 3.0f,
        .measurement_offset = 0.9945f,
        .v_empty = 3.30f,
        .v_full = 4.20f,
        .samples = 8};

    esp_err_t err = battery_drv_init(&cfg, &s_bat);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery_drv_init failed: %s", esp_err_to_name(err));
        s_bat = NULL;
    }
}

static void apply_gps_visual(ui_status_bar_t *bar)
{
    if (!bar || !bar->gps_icon) {
        return;
    }
    uint8_t bars = (bar->gps_bars_count > 4) ? 4 : bar->gps_bars_count;
    lv_color_t sig_color;
    if (!bar->gps_connected || bars == 0 || bar->gps_stale) {
        sig_color = lv_palette_main(LV_PALETTE_RED);
    } else if (bars <= 2) {
        sig_color = lv_palette_main(LV_PALETTE_YELLOW);
    } else {
        sig_color = lv_palette_main(LV_PALETTE_GREEN);
    }

    lv_color_t dim = lv_color_make(80, 80, 80);
    for (int i = 0; i < 4; i++) {
        if (!bar->gps_bars[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(bar->gps_bars[i], (i < (int)bars) ? sig_color : dim, 0);
    }
}

static void apply_ble(ui_status_bar_t *bar)
{
    if (!bar || !bar->ble_label) {
        return;
    }
    if (bar->sensor_link == SENSOR_LINK_NONE) {
        lv_obj_add_flag(bar->ble_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(bar->ble_label, LV_OBJ_FLAG_HIDDEN);
    lv_color_t color = (bar->sensor_link == SENSOR_LINK_LIVE)
                           ? ui_theme_color_accent()
                           : lv_palette_main(LV_PALETTE_RED);
    lv_obj_set_style_text_color(bar->ble_label, color, 0);
}

static void apply_rec_lock(ui_status_bar_t *bar)
{
    if (bar->rec_dot) {
        lv_obj_set_style_bg_color(bar->rec_dot,
                                  bar->recording ? ui_theme_color_rec() : lv_color_make(80, 80, 80),
                                  0);
        lv_obj_set_style_bg_opa(bar->rec_dot, bar->recording ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if (bar->lock_label) {
        label_set_text_if_changed(bar->lock_label,
                                  bar->touch_locked ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
        ui_theme_apply_label(bar->lock_label, !bar->touch_locked);
    }
}

static void status_bar_clock_update(ui_status_bar_t *bar)
{
    if (!bar || !bar->time_label) {
        return;
    }

    time_t now = time(NULL);
    if (now < 100000) {
        status_bar_set_time_placeholder(bar);
        return;
    }
    struct tm tm_info;
    if (!localtime_r(&now, &tm_info)) {
        status_bar_set_time_placeholder(bar);
        return;
    }
    char buf[16];
    if (status_bar_is_landscape()) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 tm_info.tm_hour, tm_info.tm_min);
    }
    label_set_text_if_changed(bar->time_label, buf);
}

static void status_bar_set_batt_text(ui_status_bar_t *bar, int pct)
{
    if (!bar || !bar->batt_label) {
        return;
    }
    char buf[8];
    if (pct < 0) {
        label_set_text_if_changed(bar->batt_label, "--%");
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    snprintf(buf, sizeof(buf), "%d%%", pct);
    label_set_text_if_changed(bar->batt_label, buf);
}

static void shared_batt_timer_cb(lv_timer_t *t)
{
    (void)t;
    int pct = -1;
    if (s_bat && battery_drv_read_percent(s_bat, &pct) == ESP_OK) {
        if (pct < 0) {
            pct = 0;
        }
        if (pct > 100) {
            pct = 100;
        }
        s_cached_batt_pct = pct;
    } else {
        s_cached_batt_pct = -1;
    }
    for (uint8_t i = 0; i < s_bar_count; i++) {
        if (!s_bars[i] || !s_bars[i]->root || !lv_obj_is_visible(s_bars[i]->root)) {
            continue;
        }
        status_bar_set_batt_text(s_bars[i], s_cached_batt_pct);
    }
}

static void shared_clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (uint8_t i = 0; i < s_bar_count; i++) {
        if (!s_bars[i] || !s_bars[i]->root || !lv_obj_is_visible(s_bars[i]->root)) {
            continue;
        }
        if (s_bars[i]->kind == UI_STATUS_BAR_FULL) {
            status_bar_clock_update(s_bars[i]);
        }
    }
}

static void ensure_shared_timers(void)
{
    status_bar_battery_init_once();
    if (!s_shared_batt) {
        s_shared_batt = lv_timer_create(shared_batt_timer_cb, 5000, NULL);
        shared_batt_timer_cb(s_shared_batt);
    }
    if (!s_shared_clock) {
        s_shared_clock = lv_timer_create(shared_clock_timer_cb, 1000, NULL);
    }
}

static void build_gps_widget(ui_status_bar_t *bar, int grid_col)
{
    bar->gps_cont = lv_obj_create(bar->root);
    lv_obj_remove_style_all(bar->gps_cont);
    lv_obj_clear_flag(bar->gps_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bar->gps_cont, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(bar->gps_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(bar->gps_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar->gps_cont, 0, 0);
    lv_obj_set_style_pad_all(bar->gps_cont, 0, 0);
    lv_obj_set_style_pad_gap(bar->gps_cont, 4, 0);
    lv_obj_set_layout(bar->gps_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bar->gps_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar->gps_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (grid_col >= 0) {
        lv_obj_set_grid_cell(bar->gps_cont, LV_GRID_ALIGN_STRETCH, grid_col, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    }

    bar->gps_icon = lv_label_create(bar->gps_cont);
    ui_theme_apply_label(bar->gps_icon, true);
    lv_label_set_text(bar->gps_icon, LV_SYMBOL_GPS);

    lv_obj_t *bars_cont = lv_obj_create(bar->gps_cont);
    lv_obj_remove_style_all(bars_cont);
    lv_obj_set_style_bg_opa(bars_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bars_cont, 0, 0);
    lv_obj_set_style_pad_all(bars_cont, 0, 0);
    lv_obj_set_style_pad_column(bars_cont, 2, 0);
    lv_obj_set_size(bars_cont, 18, 12);
    lv_obj_set_layout(bars_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(bars_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars_cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bars_cont, LV_OBJ_FLAG_SCROLLABLE);

    static const lv_coord_t bar_h[4] = {4, 6, 9, 12};
    for (int i = 0; i < 4; i++) {
        bar->gps_bars[i] = lv_obj_create(bars_cont);
        lv_obj_remove_style_all(bar->gps_bars[i]);
        lv_obj_set_size(bar->gps_bars[i], 3, bar_h[i]);
        lv_obj_set_style_radius(bar->gps_bars[i], 1, 0);
        lv_obj_set_style_bg_opa(bar->gps_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar->gps_bars[i], 0, 0);
        lv_obj_clear_flag(bar->gps_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

void ui_status_bar_create_ex(ui_status_bar_t *bar, lv_obj_t *parent, ui_status_bar_kind_t kind)
{
    if (!bar || !parent) {
        return;
    }

    memset(bar, 0, sizeof(*bar));
    bar->kind = kind;

    bar->root = lv_obj_create(parent);
    ui_theme_apply_surface(bar->root);
    lv_obj_clear_flag(bar->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bar->root, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(bar->root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_width(bar->root, lv_pct(100));
    lv_obj_set_height(bar->root, kind == UI_STATUS_BAR_COMPACT ? UI_STATUS_BAR_COMPACT_H : UI_STATUS_BAR_FULL_H);
    lv_obj_set_style_radius(bar->root, 0, 0);
    lv_obj_set_style_border_width(bar->root, 0, 0);
    lv_obj_set_style_pad_hor(bar->root, 6, 0);
    lv_obj_set_style_pad_ver(bar->root, 1, 0);
    lv_obj_add_flag(bar->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(bar->root, LV_LAYOUT_GRID);

    if (kind == UI_STATUS_BAR_COMPACT) {
        bar->rec_dot = lv_obj_create(bar->root);
        lv_obj_remove_style_all(bar->rec_dot);
        lv_obj_set_size(bar->rec_dot, 8, 8);
        lv_obj_set_style_radius(bar->rec_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(bar->rec_dot, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bar->rec_dot, lv_color_make(80, 80, 80), 0);
        lv_obj_clear_flag(bar->rec_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_grid_cell(bar->rec_dot, LV_GRID_ALIGN_CENTER, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

        bar->lock_label = lv_label_create(bar->root);
        ui_theme_apply_label(bar->lock_label, true);
        lv_label_set_text(bar->lock_label, LV_SYMBOL_EYE_OPEN);
        lv_obj_set_style_text_font(bar->lock_label, ui_font_caption(), 0);
        lv_obj_set_grid_cell(bar->lock_label, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);

        bar->ble_label = lv_label_create(bar->root);
        lv_label_set_text(bar->ble_label, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_font(bar->ble_label, ui_font_caption(), 0);
        lv_obj_add_flag(bar->ble_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_grid_cell(bar->ble_label, LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

        build_gps_widget(bar, 3);

        bar->batt_label = lv_label_create(bar->root);
        lv_label_set_text(bar->batt_label, "--%");
        ui_theme_apply_label(bar->batt_label, true);
        lv_obj_set_style_text_font(bar->batt_label, ui_font_caption(), 0);
        lv_obj_set_style_text_align(bar->batt_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_grid_cell(bar->batt_label, LV_GRID_ALIGN_STRETCH, 4, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    } else {
        lv_obj_set_style_bg_opa(bar->root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar->root, 0, 0);
        lv_obj_set_style_radius(bar->root, 0, 0);

        bar->title_btn = lv_obj_create(bar->root);
        lv_obj_remove_style_all(bar->title_btn);
        lv_obj_set_height(bar->title_btn, UI_STATUS_BAR_FULL_H);
        lv_obj_set_flex_grow(bar->title_btn, 1);
        lv_obj_set_flex_flow(bar->title_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar->title_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(bar->title_btn, 2, 0);
        lv_obj_add_flag(bar->title_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(bar->title_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bar->title_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(bar->title_btn, title_back_cb, LV_EVENT_CLICKED, bar);

        bar->back_icon = lv_label_create(bar->title_btn);
        lv_label_set_text(bar->back_icon, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(bar->back_icon, ui_font_caption(), 0);
        ui_theme_apply_label(bar->back_icon, false);

        bar->title_label = lv_label_create(bar->title_btn);
        ui_theme_apply_label(bar->title_label, false);
        lv_obj_set_style_text_font(bar->title_label, ui_font_caption(), 0);
        lv_label_set_long_mode(bar->title_label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(bar->title_label, 1);
        lv_label_set_text(bar->title_label, "");

        bar->time_label = lv_label_create(bar->root);
        ui_theme_apply_label(bar->time_label, false);
        lv_obj_set_style_text_font(bar->time_label, ui_font_caption(), 0);
        lv_label_set_long_mode(bar->time_label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(bar->time_label, 1);
        lv_label_set_text(bar->time_label, "--:--");

        lv_obj_t *ind = lv_obj_create(bar->root);
        lv_obj_remove_style_all(ind);
        lv_obj_set_height(ind, UI_STATUS_BAR_FULL_H);
        lv_obj_set_flex_flow(ind, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ind, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(ind, 6, 0);
        lv_obj_clear_flag(ind, LV_OBJ_FLAG_SCROLLABLE);

        build_gps_widget(bar, -1);
        lv_obj_set_parent(bar->gps_cont, ind);

        bar->ble_label = lv_label_create(ind);
        lv_label_set_text(bar->ble_label, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_font(bar->ble_label, ui_font_caption(), 0);
        lv_obj_add_flag(bar->ble_label, LV_OBJ_FLAG_HIDDEN);

        bar->batt_label = lv_label_create(ind);
        lv_label_set_text(bar->batt_label, "--%");
        ui_theme_apply_label(bar->batt_label, true);
        lv_obj_set_style_text_font(bar->batt_label, ui_font_caption(), 0);
    }

    register_bar(bar);
    ui_status_bar_set_default(bar);
    bar->gps_connected = s_cached_gps_connected;
    bar->gps_bars_count = s_cached_gps_bars;
    bar->gps_stale = s_cached_gps_stale;
    apply_gps_visual(bar);
    apply_rec_lock(bar);
    status_bar_set_batt_text(bar, s_cached_batt_pct);

    ensure_shared_timers();
    bar->orient = UI_ORIENT_PORTRAIT_0;
    status_bar_apply_layout(bar);
    if (bar->time_label) {
        status_bar_clock_update(bar);
    }
}

void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent)
{
    ui_status_bar_create_ex(bar, parent, UI_STATUS_BAR_FULL);
}

void ui_status_bar_set_title(ui_status_bar_t *bar, const char *title, ui_page_t back_page)
{
    if (!bar || !bar->title_btn || !bar->title_label) {
        return;
    }
    bar->has_back = true;
    bar->back_page = back_page;
    lv_label_set_text(bar->title_label, title ? title : "");
    lv_obj_clear_flag(bar->title_btn, LV_OBJ_FLAG_HIDDEN);
    if (bar->time_label) {
        lv_obj_add_flag(bar->time_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_status_bar_apply_theme(ui_status_bar_t *bar)
{
    if (!bar) {
        return;
    }
    if (bar->root) {
        if (bar->kind == UI_STATUS_BAR_FULL) {
            lv_obj_set_style_bg_opa(bar->root, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(bar->root, 0, 0);
            lv_obj_set_style_radius(bar->root, 0, 0);
        } else {
            ui_theme_apply_surface(bar->root);
            lv_obj_set_style_radius(bar->root, 0, 0);
            lv_obj_set_style_border_width(bar->root, 0, 0);
        }
    }
    if (bar->title_label) {
        ui_theme_apply_label(bar->title_label, false);
    }
    if (bar->back_icon) {
        ui_theme_apply_label(bar->back_icon, false);
    }
    if (bar->time_label) {
        ui_theme_apply_label(bar->time_label, false);
    }
    if (bar->gps_icon) {
        ui_theme_apply_label(bar->gps_icon, true);
    }
    if (bar->batt_label) {
        ui_theme_apply_label(bar->batt_label, true);
    }
    apply_gps_visual(bar);
    apply_rec_lock(bar);
    apply_ble(bar);
}

void ui_status_bar_set_orientation(ui_status_bar_t *bar, ui_orientation_t o)
{
    if (!bar || !bar->root) {
        return;
    }
    bar->orient = o;
    status_bar_apply_layout(bar);
    if (bar->time_label) {
        status_bar_clock_update(bar);
    }
}

void ui_status_bar_set_gps_status(ui_status_bar_t *bar, bool connected, uint8_t bars_0_to_4)
{
    if (!bar || !bar->gps_icon) {
        return;
    }
    bar->gps_connected = connected;
    bar->gps_bars_count = (bars_0_to_4 > 4) ? 4 : bars_0_to_4;
    apply_gps_visual(bar);
}

void ui_status_bar_set_battery(ui_status_bar_t *bar, int percent)
{
    status_bar_set_batt_text(bar, percent);
}

void ui_status_bar_set_time_base(ui_status_bar_t *bar, uint32_t start_sec)
{
    (void)start_sec;
    if (bar) {
        status_bar_clock_update(bar);
    }
}

lv_obj_t *ui_status_bar_root(ui_status_bar_t *bar)
{
    return bar ? bar->root : NULL;
}

void ui_status_bar_force_refresh(ui_status_bar_t *bar)
{
    if (!bar) {
        return;
    }
    status_bar_clock_update(bar);
}

void ui_status_bar_set_gps_default_safe(bool connected, uint8_t bars_0_to_4)
{
    s_cached_gps_connected = connected;
    s_cached_gps_bars = (bars_0_to_4 > 4) ? 4 : bars_0_to_4;
    if (s_bar_count == 0) {
        return;
    }
    lvgl_port_lock(0);
    for (uint8_t i = 0; i < s_bar_count; i++) {
        ui_status_bar_set_gps_status(s_bars[i], s_cached_gps_connected, s_cached_gps_bars);
    }
    lvgl_port_unlock();
}

void ui_status_bar_apply_snapshot(const coach_ui_snapshot_t *snap)
{
    if (!snap) {
        return;
    }
    s_cached_gps_connected = snap->gps_connected;
    s_cached_gps_bars = snap->gps_bars;
    s_cached_gps_stale = snap->gps_stale;
    if (snap->battery_pct <= 100) {
        s_cached_batt_pct = (int)snap->battery_pct;
    }
    for (uint8_t i = 0; i < s_bar_count; i++) {
        ui_status_bar_t *bar = s_bars[i];
        if (!bar || !bar->root || !lv_obj_is_visible(bar->root)) {
            continue;
        }
        bool gps_chg = (bar->gps_connected != snap->gps_connected) ||
                       (bar->gps_bars_count != snap->gps_bars) ||
                       (bar->gps_stale != snap->gps_stale);
        bool rec_chg = (bar->recording != snap->recording) ||
                       (bar->touch_locked != snap->touch_locked);
        bar->gps_connected = snap->gps_connected;
        bar->gps_bars_count = snap->gps_bars;
        bar->gps_stale = snap->gps_stale;
        bar->recording = snap->recording;
        bar->touch_locked = snap->touch_locked;
        if (gps_chg) {
            apply_gps_visual(bar);
        }
        if (rec_chg) {
            apply_rec_lock(bar);
        }
        if (snap->battery_pct <= 100) {
            status_bar_set_batt_text(bar, (int)snap->battery_pct);
        }
        if (bar->sensor_link != snap->sensor_link) {
            bar->sensor_link = snap->sensor_link;
            apply_ble(bar);
        }
    }
}
