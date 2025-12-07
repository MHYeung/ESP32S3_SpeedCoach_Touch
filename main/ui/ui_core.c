// main/ui/ui_core.c
#include "ui.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <string.h>

/* Per-page builders */
#include "ui_controls_page.h"
#include "ui_imu_page.h"
#include "ui_system_page.h"
#include "ui_settings_page.h"
#include "ui_sd_test_page.h"

/* Global LVGL handles for the UI core */
static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_tabview = NULL;
static lv_obj_t *s_tab_controls = NULL;
static lv_obj_t *s_tab_imu = NULL;
static lv_obj_t *s_tab_system = NULL;
static lv_obj_t *s_tab_settings = NULL;
static lv_obj_t *s_tab_sd_test = NULL;

/* Callbacks registered from main.c */

static ui_dark_mode_cb_t s_dark_mode_cb = NULL;
static ui_auto_rotate_cb_t s_auto_rotate_cb = NULL;

/* ---------- Registration from main.c ---------- */



void ui_register_dark_mode_cb(ui_dark_mode_cb_t cb)
{
    s_dark_mode_cb = cb;
}

void ui_register_auto_rotate_cb(ui_auto_rotate_cb_t cb)
{
    s_auto_rotate_cb = cb;
}

/* These are called by Settings page event handlers */
void ui_notify_dark_mode_changed(bool enabled)
{
    if (s_dark_mode_cb)
    {
        s_dark_mode_cb(enabled);
    }
}

void ui_notify_auto_rotate_changed(bool enabled)
{
    if (s_auto_rotate_cb)
    {
        s_auto_rotate_cb(enabled);
    }
}

/* ---------- Orientation + navigation ---------- */

void ui_set_orientation(ui_orientation_t o)
{
    lv_display_rotation_t rot = LV_DISPLAY_ROTATION_0;

    switch (o)
    {
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
    lv_display_set_rotation(s_disp, rot);
    lvgl_port_unlock();
}

void ui_go_to_page(ui_page_t page, bool animated)
{
    if (!s_tabview)
        return;
    if (page < 0 || page >= UI_PAGE_COUNT)
        return;

    lvgl_port_lock(0);
    lv_tabview_set_active(
        s_tabview,
        (uint32_t)page,
        animated ? LV_ANIM_ON : LV_ANIM_OFF);
    lvgl_port_unlock();
}

/* ---------- Tab creation ---------- */

static void create_tabs_ui(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);

    s_tabview = lv_tabview_create(scr);
    lv_tabview_set_tab_bar_position(s_tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(s_tabview, 28);

    /* Make the tab bar scrollable horizontally */
    lv_obj_t *tab_bar = lv_tabview_get_tab_bar(s_tabview);
    lv_obj_set_scroll_dir(tab_bar, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(tab_bar, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_x(tab_bar, LV_SCROLL_SNAP_CENTER);

    s_tab_controls = lv_tabview_add_tab(s_tabview, "Controls");
    s_tab_imu = lv_tabview_add_tab(s_tabview, "IMU");
    s_tab_system = lv_tabview_add_tab(s_tabview, "System");
    s_tab_settings = lv_tabview_add_tab(s_tabview, "Settings");
    s_tab_sd_test = lv_tabview_add_tab(s_tabview, "SD");

    /* Make each tab button width fit its content (label text) */
    uint32_t tab_cnt = lv_obj_get_child_count(tab_bar);
    for (uint32_t i = 0; i < tab_cnt; i++) {
        lv_obj_t *tab_btn = lv_obj_get_child(tab_bar, i);

        /* Don't let it stretch */
        lv_obj_set_flex_grow(tab_btn, 0);

        /* Let width be defined by content */
        lv_obj_set_width(tab_btn, LV_SIZE_CONTENT);

        /* Optional: give some horizontal padding so labels don't touch */
        lv_obj_set_style_pad_hor(tab_btn, 8, 0);
    }

    controls_page_create(s_tab_controls);
    imu_page_create(s_tab_imu);
    system_page_create(s_tab_system);
    settings_page_create(s_tab_settings);
    sd_test_page_create(s_tab_sd_test);
}
/* ---------- Public init ---------- */

void ui_init(lv_disp_t *disp)
{
    s_disp = disp;

    lvgl_port_lock(0);
    create_tabs_ui();
    lvgl_port_unlock();
}
