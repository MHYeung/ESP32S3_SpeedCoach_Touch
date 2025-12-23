// main/ui/ui_status_bar.c
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "rtc_pcf85063.h"

#include "esp_err.h"

#include <stdio.h>
#include <string.h>

static void status_bar_clock_update(ui_status_bar_t *bar)
{
    if (!bar || !bar->time_label) return;

    // --- Preferred: RTC time ---
    datetime_t rtc;
    esp_err_t err = PCF85063_read_time(&rtc);
    if (err == ESP_OK) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                 (unsigned)rtc.hour, (unsigned)rtc.minute, (unsigned)rtc.second);
        lv_label_set_text(bar->time_label, buf);
        return;
    }

    // --- Fallback: old tick-based clock (in case RTC not ready) ---
    uint32_t now_ms = lv_tick_get();
    uint32_t elapsed_s = (now_ms - bar->clock_start_ms) / 1000;
    uint32_t t = bar->clock_start_sec + elapsed_s;

    uint32_t hh = (t / 3600) % 24;
    uint32_t mm = (t / 60) % 60;
    uint32_t ss = t % 60;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", (unsigned)hh, (unsigned)mm, (unsigned)ss);
    lv_label_set_text(bar->time_label, buf);
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

    /* 3-column grid: time | GPS | battery */
    static int32_t s_hdr_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t s_hdr_rows[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_layout(bar->root, LV_LAYOUT_GRID);
    lv_obj_set_grid_dsc_array(bar->root, s_hdr_cols, s_hdr_rows);

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
    lv_label_set_text(bar->batt_label, "BAT: --%");
    ui_theme_apply_label(bar->batt_label, true);
    lv_obj_add_flag(bar->batt_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_label_set_long_mode(bar->batt_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(bar->batt_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_grid_cell(bar->batt_label, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);

    bar->clock_start_ms = lv_tick_get();
    bar->clock_start_sec = 0; /* Dummy start: 12:00:00 */
    bar->clock_timer = lv_timer_create(status_bar_clock_timer_cb, 1000, bar);
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

void ui_status_bar_set_battery_text(ui_status_bar_t *bar, const char *text)
{
    if (!bar || !bar->batt_label) return;
    lv_label_set_text(bar->batt_label, text ? text : "");
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
