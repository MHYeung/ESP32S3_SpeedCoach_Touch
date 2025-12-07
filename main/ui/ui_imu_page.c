// main/ui/ui_imu_page.c
#include "ui_imu_page.h"
#include "ui.h"               // for ui_update_imu declaration
#include "esp_lvgl_port.h"
#include <stdio.h>

/* Private IMU UI objects */
static lv_obj_t *s_label_imu = NULL;
static lv_obj_t *s_chart     = NULL;
static lv_chart_series_t *s_ser_x = NULL;
static lv_chart_series_t *s_ser_y = NULL;
static lv_chart_series_t *s_ser_z = NULL;

void imu_page_create(lv_obj_t *parent)
{
    s_label_imu = lv_label_create(parent);
    lv_label_set_text(s_label_imu, "ax=?  ay=?  az=?  m/s^2");
    lv_obj_align(s_label_imu, LV_ALIGN_TOP_MID, 0, 10);

    s_chart = lv_chart_create(parent);
    lv_obj_set_size(s_chart, lv_pct(95), lv_pct(70));
    lv_obj_align(s_chart, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, 60);
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, -2000, 2000);

    s_ser_x = lv_chart_add_series(
        s_chart, lv_palette_main(LV_PALETTE_RED),   LV_CHART_AXIS_PRIMARY_Y);
    s_ser_y = lv_chart_add_series(
        s_chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    s_ser_z = lv_chart_add_series(
        s_chart, lv_palette_main(LV_PALETTE_BLUE),  LV_CHART_AXIS_PRIMARY_Y);

    lv_chart_set_all_value(s_chart, s_ser_x, 0);
    lv_chart_set_all_value(s_chart, s_ser_y, 0);
    lv_chart_set_all_value(s_chart, s_ser_z, 0);
}

/* Public API used from main.c / imu_ui_task */
void ui_update_imu(float ax, float ay, float az)
{
    /* Convert to centi-m/s^2 for chart */
    int16_t cx = (int16_t)(ax * 100.0f);
    int16_t cy = (int16_t)(ay * 100.0f);
    int16_t cz = (int16_t)(az * 100.0f);

    lvgl_port_lock(0);

    if (s_label_imu) {
        static char buf[64];
        snprintf(buf, sizeof(buf),
                 "ax=%.2f  ay=%.2f  az=%.2f  m/s^2",
                 ax, ay, az);
        lv_label_set_text(s_label_imu, buf);
    }

    if (s_chart) {
        lv_chart_set_next_value(s_chart, s_ser_x, cx);
        lv_chart_set_next_value(s_chart, s_ser_y, cy);
        lv_chart_set_next_value(s_chart, s_ser_z, cz);
    }

    lvgl_port_unlock();
}
