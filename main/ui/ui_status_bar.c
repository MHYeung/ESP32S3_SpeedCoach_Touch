// main/ui/ui_status_bar.c
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "rtc_pcf85063.h"
#include "battery_drv.h"

#include "esp_log.h"
#include "esp_err.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_status_bar";

static battery_drv_handle_t s_bat = NULL;
static bool s_bat_inited = false;

static int32_t s_cols_land[] = { LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST };
static int32_t s_cols_port[] = { LV_GRID_FR(5), LV_GRID_FR(4), LV_GRID_FR(4), LV_GRID_TEMPLATE_LAST };
static int32_t s_rows[]      = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

static bool status_bar_is_landscape(void)
{
    lv_display_t *disp = lv_disp_get_default();
    lv_display_rotation_t r = lv_display_get_rotation(disp);
    return (r == LV_DISPLAY_ROTATION_90 || r == LV_DISPLAY_ROTATION_270);
}

static void status_bar_set_time_placeholder(ui_status_bar_t *bar)
{
    if (!bar || !bar->time_label) return;
    lv_label_set_text(bar->time_label, status_bar_is_landscape() ? "--:--:--" : "--:--");
}

static void status_bar_apply_layout(ui_status_bar_t *bar)
{
    bool land = status_bar_is_landscape();

    lv_obj_set_grid_dsc_array(bar->root, land ? s_cols_land : s_cols_port, s_rows);

    lv_obj_set_style_pad_hor(bar->root, land ? 10 : 6, 0);
    lv_obj_set_style_pad_ver(bar->root, land ? 6  : 4, 0);

    lv_label_set_long_mode(bar->time_label, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(bar->gps_label,  LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(bar->batt_label, LV_LABEL_LONG_DOT);
}


// One-time init for the whole app (since ADC channel is global/shared)
static void status_bar_battery_init_once(void)
{
    if (s_bat_inited) return;
    s_bat_inited = true;

    battery_drv_config_t cfg = {
        .unit = ADC_UNIT_1,
        .channel = ADC_CHANNEL_7,          // GPIO8 on ESP32-S3
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,

        // From your Waveshare example idea (tune later if needed)
        .divider_ratio = 3.0f,
        .measurement_offset = 0.9945f,

        // Basic % mapping (tune later)
        .v_empty = 3.30f,
        .v_full  = 4.20f,

        .samples = 8
    };

    esp_err_t err = battery_drv_init(&cfg, &s_bat);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "battery_drv_init failed: %s", esp_err_to_name(err));
        s_bat = NULL;
    }
}

static void status_bar_batt_timer_cb(lv_timer_t *t)
{
    ui_status_bar_t *bar = (ui_status_bar_t *)lv_timer_get_user_data(t);
    if (!bar || !bar->batt_label) return;

    if (!s_bat) {
        // driver not available
        lv_label_set_text(bar->batt_label, "--%");
        return;
    }

    int pct = -1;
    if (battery_drv_read_percent(s_bat, &pct) == ESP_OK) {
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_label_set_text_fmt(bar->batt_label, "%d%%", pct);
    } else {
        lv_label_set_text(bar->batt_label, "--%");
    }
}


static void status_bar_clock_update(ui_status_bar_t *bar)
{
    if (!bar || !bar->time_label) return;

    bool valid = false;
    if (PCF85063_is_time_valid(&valid) == ESP_OK && valid) {
        datetime_t rtc = {0};
        if (PCF85063_read_time(&rtc) == ESP_OK) {
            char buf[16];
            if (status_bar_is_landscape()) {
                snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                         (unsigned)rtc.hour, (unsigned)rtc.minute, (unsigned)rtc.second);
            } else {
                snprintf(buf, sizeof(buf), "%02u:%02u",
                         (unsigned)rtc.hour, (unsigned)rtc.minute);
            }
            lv_label_set_text(bar->time_label, buf);
            return;
        }
    }

    // RTC not ready (OSF set / not set / not inited)
    status_bar_set_time_placeholder(bar);
}

void ui_status_bar_set_orientation(ui_status_bar_t *bar, ui_orientation_t o)
{
    if (!bar || !bar->root) return;
    bar->orient = o;

    status_bar_apply_layout(bar);
    status_bar_clock_update(bar);   // re-render time in correct format
}

static void status_bar_clock_timer_cb(lv_timer_t *t)
{
    ui_status_bar_t *bar = (ui_status_bar_t *)lv_timer_get_user_data(t);
    status_bar_clock_update(bar);
}

void ui_status_bar_create(ui_status_bar_t *bar, lv_obj_t *parent)
{
    if (!bar || !parent) return;

    memset(bar, 0, sizeof(*bar));

    bar->root = lv_obj_create(parent);
    ui_theme_apply_surface(bar->root);
    lv_obj_set_width(bar->root, lv_pct(100));
    lv_obj_set_height(bar->root, lv_pct(10));
    lv_obj_set_style_radius(bar->root, 0, 0);
    lv_obj_set_style_border_width(bar->root, 0, 0);
    lv_obj_set_style_pad_hor(bar->root, 10, 0);
    lv_obj_set_style_pad_ver(bar->root, 6, 0);
    lv_obj_add_flag(bar->root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_layout(bar->root, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(bar->root, (status_bar_is_landscape() ? s_cols_land : s_cols_port), s_rows);

    bar->time_label = lv_label_create(bar->root);
    ui_theme_apply_label(bar->time_label, false);
    lv_obj_add_flag(bar->time_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(bar->time_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(bar->time_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_grid_cell(bar->time_label, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    bar->gps_label = lv_label_create(bar->root);
    lv_label_set_text(bar->gps_label, "");
    ui_theme_apply_label(bar->gps_label, true);
    lv_obj_add_flag(bar->gps_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_align(bar->gps_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(bar->gps_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_grid_cell(bar->gps_label, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    ui_status_bar_set_gps_status(bar, false, 0);

    bar->batt_label = lv_label_create(bar->root);
    lv_label_set_text(bar->batt_label, "--%");
    ui_theme_apply_label(bar->batt_label, true);
    lv_obj_add_flag(bar->batt_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(bar->batt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(bar->batt_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_grid_cell(bar->batt_label, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

     // Battery auto-update
    status_bar_battery_init_once();
    bar->batt_timer = lv_timer_create(status_bar_batt_timer_cb, 5000, bar); // every 5s
    status_bar_batt_timer_cb(bar->batt_timer); // immediate first refresh

    bar->clock_start_ms = lv_tick_get();
    bar->clock_start_sec = 0; /* Dummy start: 12:00:00 */
    bar->clock_timer = lv_timer_create(status_bar_clock_timer_cb, 1000, bar);
    bar->orient = UI_ORIENT_LANDSCAPE_90; // or PORTRAIT_0 if you prefer
    status_bar_apply_layout(bar);
    status_bar_clock_update(bar);
}

void ui_status_bar_apply_theme(ui_status_bar_t *bar)
{
    if (!bar) return;

    if (bar->root) {
        ui_theme_apply_surface(bar->root);
        lv_obj_set_style_radius(bar->root, 0, 0);
        lv_obj_set_style_border_width(bar->root, 0, 0);
    }
    if (bar->time_label) ui_theme_apply_label(bar->time_label, false);
    if (bar->gps_label) ui_theme_apply_label(bar->gps_label, true);
    if (bar->batt_label) ui_theme_apply_label(bar->batt_label, true);
}

void ui_status_bar_set_gps_status(ui_status_bar_t *bar, bool connected, uint8_t bars_0_to_4)
{
    if (!bar || !bar->gps_label) return;

    if (!connected) {
        lv_label_set_text(bar->gps_label, LV_SYMBOL_GPS " " LV_SYMBOL_CLOSE);
        return;
    }

    uint8_t bars = bars_0_to_4;
    if (bars > 4) bars = 4;

    if (bars == 0) {
        lv_label_set_text(bar->gps_label, LV_SYMBOL_GPS " " LV_SYMBOL_WARNING);
        return;
    }

    char bars_txt[5];
    for (uint8_t i = 0; i < bars; i++) {
        bars_txt[i] = '|';
    }
    bars_txt[bars] = '\0';

    char buf[32];
    snprintf(buf, sizeof(buf), "%s %s%s", LV_SYMBOL_GPS, LV_SYMBOL_WIFI, bars_txt);
    lv_label_set_text(bar->gps_label, buf);
}

void ui_status_bar_set_battery(ui_status_bar_t *bar, int percent)
{
    if (!bar || !bar->batt_label) return;

    if (percent < 0)  { lv_label_set_text(bar->batt_label, "--%"); return; }
    if (percent > 100) percent = 100;

    lv_label_set_text_fmt(bar->batt_label, "%d%%", percent);
}

void ui_status_bar_set_time_base(ui_status_bar_t *bar, uint32_t start_sec)
{
    if (!bar) return;
    bar->clock_start_ms = lv_tick_get();
    bar->clock_start_sec = start_sec;
    status_bar_clock_update(bar);
}

lv_obj_t *ui_status_bar_root(ui_status_bar_t *bar)
{
    return bar ? bar->root : NULL;
}

void ui_status_bar_force_refresh(ui_status_bar_t *bar)
{
    if (!bar) return;
    status_bar_clock_update(bar);
    if (bar->clock_timer) lv_timer_ready(bar->clock_timer); // immediate tick
}
