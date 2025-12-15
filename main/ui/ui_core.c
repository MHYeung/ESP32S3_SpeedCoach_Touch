// main/ui/ui_core.c
#include "ui.h"
#include "ui_data_page.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <string.h>

/* Global LVGL handles for the UI core */
static lv_disp_t *s_disp = NULL;

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

    /* data_page_set_orientation() locks internally */
    data_page_set_orientation(o);
}

void ui_go_to_page(ui_page_t page, bool animated)
{
    (void)page;
    (void)animated;
    /* Tabview is currently disabled (Data-only screen). */
}

static void create_data_ui(void)
{
    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(scr);
    data_page_create(scr);
}
/* ---------- Public init ---------- */

void ui_init(lv_disp_t *disp)
{
    s_disp = disp;

    lvgl_port_lock(0);
    create_data_ui();
    lvgl_port_unlock();
}
