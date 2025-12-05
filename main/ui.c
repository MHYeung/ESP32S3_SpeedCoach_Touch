#include "ui.h"
#include "esp_lvgl_port.h" // for lvgl_port_lock/unlock
#include <stdio.h>
#include <string.h>

/* Keep LVGL objects private to this module */
static lv_disp_t *s_disp = NULL;

static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_controls;
static lv_obj_t *s_tab_imu;

static lv_obj_t *s_label_imu;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_x;
static lv_chart_series_t *s_ser_y;
static lv_chart_series_t *s_ser_z;

/* ------------ Internal UI callbacks ------------ */

static void btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    const char *txt = lv_label_get_text(label);
    if (strcmp(txt, "Click me") == 0)
    {
        lv_label_set_text(label, "Touched!");
    }
    else
    {
        lv_label_set_text(label, "Click me");
    }
}

void ui_set_orientation(ui_orientation_t o)
{
    /* Map our enum -> LVGL’s rotation enum */
    lv_display_rotation_t rot = LV_DISPLAY_ROTATION_0;

    switch (o) {
    case UI_ORIENT_PORTRAIT_0:
        rot = LV_DISPLAY_ROTATION_0;
        break;
    case UI_ORIENT_LANDSCAPE_90:
        rot = LV_DISPLAY_ROTATION_90;
        break;
    case UI_ORIENT_PORTRAIT_180:
        rot = LV_DISPLAY_ROTATION_180;
        break;
    case UI_ORIENT_LANDSCAPE_270:
        rot = LV_DISPLAY_ROTATION_270;
        break;
    }

    lvgl_port_lock(0);
    lv_display_set_rotation(s_disp, rot);   // LVGL 9 API
    lvgl_port_unlock();
}

static void create_tabs_ui(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);

    /* LVGL 9 tabview creation */
    s_tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(s_tabview, LV_DIR_TOP); // optional, top bar

    /* Create tabs */
    s_tab_controls = lv_tabview_add_tab(s_tabview, "Controls");
    s_tab_imu = lv_tabview_add_tab(s_tabview, "IMU");

    /* -------- Tab 1: Controls -------- */
    lv_obj_t *label1 = lv_label_create(s_tab_controls);
    lv_label_set_text(label1, "Page 1: Button test");
    lv_obj_align(label1, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *btn = lv_button_create(s_tab_controls);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click me");
    lv_obj_center(btn_label);

    /* -------- Tab 2: IMU -------- */
    s_label_imu = lv_label_create(s_tab_imu);
    lv_label_set_text(s_label_imu, "ax=?  ay=?  az=?  m/s^2");
    lv_obj_align(s_label_imu, LV_ALIGN_TOP_MID, 0, 10);

    s_chart = lv_chart_create(s_tab_imu);
    lv_obj_set_size(s_chart, lv_pct(95), lv_pct(70));
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 60);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -2000, 2000);

    s_ser_x = lv_chart_add_series(s_chart,
                                  lv_palette_main(LV_PALETTE_RED),
                                  LV_CHART_AXIS_PRIMARY_Y);
    s_ser_y = lv_chart_add_series(s_chart,
                                  lv_palette_main(LV_PALETTE_GREEN),
                                  LV_CHART_AXIS_PRIMARY_Y);
    s_ser_z = lv_chart_add_series(s_chart,
                                  lv_palette_main(LV_PALETTE_BLUE),
                                  LV_CHART_AXIS_PRIMARY_Y);

    lv_chart_set_all_value(s_chart, s_ser_x, 0);
    lv_chart_set_all_value(s_chart, s_ser_y, 0);
    lv_chart_set_all_value(s_chart, s_ser_z, 0);
}

/* ------------ Public API ------------ */

void ui_init(lv_disp_t *disp)
{
    s_disp = disp;

    /* UI creation must hold the LVGL lock */
    lvgl_port_lock(0);
    create_tabs_ui();
    lvgl_port_unlock();
}

void ui_update_imu(float ax, float ay, float az)
{
    /* Convert to centi-m/s^2 for the chart */
    int16_t cx = (int16_t)(ax * 100.0f);
    int16_t cy = (int16_t)(ay * 100.0f);
    int16_t cz = (int16_t)(az * 100.0f);

    lvgl_port_lock(0);

    if (s_label_imu)
    {
        static char buf[64];
        snprintf(buf, sizeof(buf),
                 "ax=%.2f  ay=%.2f  az=%.2f  m/s^2",
                 ax, ay, az);
        lv_label_set_text(s_label_imu, buf);
    }

    if (s_chart)
    {
        lv_chart_set_next_value(s_chart, s_ser_x, cx);
        lv_chart_set_next_value(s_chart, s_ser_y, cy);
        lv_chart_set_next_value(s_chart, s_ser_z, cz);
    }

    lvgl_port_unlock();
}
