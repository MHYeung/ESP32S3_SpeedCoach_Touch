#include "ui_core_internal.h"

#include "app_context.h"
#include "coach_ui_snapshot.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "interval_program/interval_program.h"
#include "lcd_st7789.h"
#include "nvs_helper.h"
#include "race_program/race_program.h"
#include "ui_activity_detail_page.h"
#include "ui_activity_summary_page.h"
#include "ui_data_page.h"
#include "ui_interval_data_page.h"
#include "ui_interval_setup_page.h"
#include "ui_menu_page.h"
#include "ui_race_data_page.h"
#include "ui_race_setup_page.h"
#include "ui_settings_page.h"
#include "ui_step_setup_page.h"
#include "ui_sensors_page.h"
#include "ui_theme.h"

#include <math.h>
#include <string.h>

static uint8_t s_brightness = 80;
static bool s_auto_dim = true;
static uint8_t s_applied_brightness = 0xFF;
static volatile bool s_display_sleep = false;

static void apply_backlight(uint8_t percent)
{
    if (percent == s_applied_brightness) {
        return;
    }
    s_applied_brightness = percent;
    lcd_st7789_set_backlight_percent(percent);
}

static void snapshot_idle(coach_ui_snapshot_t *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->time_s = NAN;
    snap->distance_m = NAN;
    snap->pace_s_per_500m = NAN;
    snap->avg_pace_s_per_500m = NAN;
    snap->speed_mps = NAN;
    snap->spm = NAN;
    snap->stroke_len_m = NAN;
    snap->split_progress_m = NAN;
    snap->last_split_pace_s = NAN;
    snap->last_split_delta_s = NAN;
}

static void overlay_session_time(coach_ui_snapshot_t *snap)
{
    if (!s_activity_recording || s_session_last_us <= 0) {
        return;
    }
    snap->recording = true;
    snap->time_s = (float)(esp_timer_get_time() - s_session_last_us) * 1e-6f;
}

static void coach_ui_tick(lv_timer_t *t)
{
    (void)t;

    static coach_ui_snapshot_t s_held;
    static bool s_held_valid;
    static uint32_t s_applied_seq;
    static bool s_applied_once;

    coach_ui_snapshot_t snap;
    bool copied = coach_ui_snapshot_copy(&snap);
    if (copied) {
        s_held = snap;
        s_held_valid = true;
    } else if (s_held_valid) {
        snap = s_held;
    } else {
        snapshot_idle(&snap);
    }
    overlay_session_time(&snap);

    bool full = !s_applied_once || (copied && snap.seq != s_applied_seq);
    if (full) {
        s_applied_once = true;
        s_applied_seq = snap.seq;
        data_page_apply_snapshot(&snap);
        interval_data_page_apply_snapshot(&snap);
        race_data_page_apply_snapshot(&snap);
        ui_status_bar_apply_snapshot(&snap);
    } else {
        data_page_set_time_s(snap.time_s);
    }

    if (s_display_sleep) {
        if (lv_disp_get_inactive_time(NULL) < 250) {
            s_display_sleep = false;
            apply_backlight(s_brightness);
        }
        return;
    }

    bool dim = s_auto_dim && snap.recording &&
               (lv_disp_get_inactive_time(NULL) > 15000);
    if (dim) {
        s_display_sleep = true;
        apply_backlight(0);
        return;
    }
    apply_backlight(s_brightness);
}

bool ui_is_landscape(void)
{
    if (!ui_s_disp)
        return false;
    lv_display_rotation_t r = lv_display_get_rotation(ui_s_disp);
    return (r == LV_DISPLAY_ROTATION_90 || r == LV_DISPLAY_ROTATION_270);
}

void ui_register_stop_save_confirm_cb(ui_stop_save_confirm_cb_t cb) { ui_s_stop_save_confirm_cb = cb; }
void ui_register_shutdown_confirm_cb(ui_shutdown_confirm_cb_t cb) { ui_s_shutdown_confirm_cb = cb; }
void ui_register_dark_mode_cb(ui_dark_mode_cb_t cb) { ui_s_dark_mode_cb = cb; }
void ui_register_auto_rotate_cb(ui_auto_rotate_cb_t cb) { ui_s_auto_rotate_cb = cb; }

void ui_set_interval_start_armed(bool armed) { ui_s_interval_start_armed = armed; }
bool ui_take_interval_start_armed(void)
{
    bool armed = ui_s_interval_start_armed;
    ui_s_interval_start_armed = false;
    return armed;
}

void ui_set_interval_data_visible(bool visible)
{
    ui_s_interval_data_visible = visible;

    if (!visible && ui_s_current_page == UI_INTERVAL_DATA_PAGE)
    {
        ui_go_to_page(UI_PAGE_DATA, true);
    }
}

void ui_set_race_start_armed(bool armed) { ui_s_race_start_armed = armed; }
bool ui_take_race_start_armed(void)
{
    bool armed = ui_s_race_start_armed;
    ui_s_race_start_armed = false;
    return armed;
}

void ui_set_race_data_visible(bool visible)
{
    ui_s_race_data_visible = visible;

    if (!visible && ui_s_current_page == UI_RACE_DATA_PAGE)
    {
        /* ui_go_to_page has no animated RACE_DATA -> DATA path; instant relayout. */
        ui_go_to_page(UI_PAGE_DATA, false);
    }
}

void ui_set_step_start_armed(bool armed) { ui_s_step_start_armed = armed; }
bool ui_take_step_start_armed(void)
{
    bool armed = ui_s_step_start_armed;
    ui_s_step_start_armed = false;
    return armed;
}

void ui_notify_dark_mode_changed(bool enabled)
{
    ui_set_dark_mode(enabled);
    if (ui_s_dark_mode_cb)
        ui_s_dark_mode_cb(enabled);
}

void ui_notify_auto_rotate_changed(bool enabled)
{
    if (ui_s_auto_rotate_cb)
        ui_s_auto_rotate_cb(enabled);
}

void ui_set_dark_mode(bool enabled)
{
    ui_s_dark_mode = enabled;
    lvgl_port_lock(0);
    /* Switch ON (enabled) must look dark on this panel; palettes were reversed. */
    ui_theme_set(enabled ? UI_THEME_LIGHT : UI_THEME_DARK);
    data_page_apply_theme();
    settings_page_apply_theme();
    menu_page_apply_theme();
    activity_summary_page_apply_theme();
    activity_detail_page_apply_theme();
    interval_setup_page_apply_theme();
    interval_data_page_apply_theme();
    race_setup_page_apply_theme();
    race_data_page_apply_theme();
    step_setup_page_apply_theme();
    sensors_page_apply_theme();
    if (ui_status_bar_root(&ui_s_live_status)) {
        ui_status_bar_apply_theme(&ui_s_live_status);
    }
    lvgl_port_unlock();
}

bool ui_get_dark_mode(void) { return ui_s_dark_mode; }

ui_page_t ui_get_current_page(void)
{
    return ui_s_current_page;
}

bool ui_is_modal_active(void)
{
    return ui_s_modal_active;
}

void ui_set_touch_lock(bool locked)
{
    ui_s_touch_locked = locked;
}

bool ui_is_touch_lock(void)
{
    return ui_s_touch_locked;
}

void ui_toggle_touch_lock(void)
{
    ui_s_touch_locked = !ui_s_touch_locked;
}

void ui_set_brightness_percent(uint8_t percent)
{
    if (percent < 10) {
        percent = 10;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
    apply_backlight(percent);
}

uint8_t ui_get_brightness_percent(void)
{
    return s_brightness;
}

void ui_set_auto_dim(bool enabled)
{
    s_auto_dim = enabled;
    if (!enabled) {
        apply_backlight(s_brightness);
    }
}

bool ui_get_auto_dim(void)
{
    return s_auto_dim;
}

void ui_set_display_sleep(bool sleep)
{
    s_display_sleep = sleep;
    apply_backlight(sleep ? 0 : s_brightness);
}

void ui_toggle_display_sleep(void)
{
    ui_set_display_sleep(!s_display_sleep);
}

bool ui_is_display_sleep(void)
{
    return s_display_sleep;
}

void ui_notify_user_activity(void)
{
    lvgl_port_lock(0);
    lv_disp_trig_activity(NULL);
    if (s_display_sleep) {
        s_display_sleep = false;
        apply_backlight(s_brightness);
    }
    lvgl_port_unlock();
}

void ui_init(lv_disp_t *disp)
{
    ui_s_disp = disp;
    coach_ui_snapshot_init();
    s_brightness = nvs_helper_get_brightness();
    s_auto_dim = nvs_helper_get_auto_dim();
    apply_backlight(s_brightness);

    lvgl_port_lock(0);
    ui_theme_init(ui_s_disp);
    create_pages_ui();
    interval_program_init();
    race_program_init();
    lv_timer_create(coach_ui_tick, 100, NULL);
    lvgl_port_unlock();
}
