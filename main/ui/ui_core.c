// main/ui/ui_core.c
#include "ui.h"
#include "ui_data_page.h"
#include "ui_settings_page.h"
#include "ui_theme.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <string.h>

/* Global LVGL handles for the UI core */
static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_page_data = NULL;
static lv_obj_t *s_page_settings = NULL;
static lv_obj_t *s_top_gesture = NULL;
static lv_obj_t *s_settings_bottom_gesture = NULL;
static ui_page_t s_current_page = UI_PAGE_DATA;
static bool s_transitioning = false;
static bool s_top_swipe_armed = false;
static lv_point_t s_top_swipe_sum = {0};
static bool s_settings_swipe_armed = false;
static lv_point_t s_settings_swipe_sum = {0};

static ui_shutdown_confirm_cb_t s_shutdown_confirm_cb = NULL;
static lv_obj_t *s_shutdown_overlay = NULL;

static void ui_pages_relayout(void);

/* Callbacks registered from main.c */

static ui_dark_mode_cb_t s_dark_mode_cb = NULL;
static ui_auto_rotate_cb_t s_auto_rotate_cb = NULL;
static bool s_dark_mode = true;

/* ---------- Registration from main.c ---------- */

void ui_register_shutdown_confirm_cb(ui_shutdown_confirm_cb_t cb)
{
    s_shutdown_confirm_cb = cb;
}

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
    ui_set_dark_mode(enabled);
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

void ui_set_dark_mode(bool enabled)
{
    s_dark_mode = enabled;

    lvgl_port_lock(0);
    ui_theme_set(enabled ? UI_THEME_DARK : UI_THEME_LIGHT);

    /* Re-apply theme to any already-created UI pages */
    data_page_apply_theme();
    settings_page_apply_theme();
    lvgl_port_unlock();
}

bool ui_get_dark_mode(void)
{
    return s_dark_mode;
}

/* ---------- Orientation + navigation ---------- */

static void top_swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (!indev) return;

    if (s_transitioning) return;
    if (s_current_page != UI_PAGE_DATA) return;

    if (code == LV_EVENT_PRESSED) {
        s_top_swipe_sum.x = 0;
        s_top_swipe_sum.y = 0;
        s_top_swipe_armed = true;
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        s_top_swipe_armed = false;
        return;
    }

    if (code != LV_EVENT_PRESSING) return;
    if (!s_top_swipe_armed) return;

    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    s_top_swipe_sum.x += v.x;
    s_top_swipe_sum.y += v.y;

    const int32_t min_px = 30;
    if (s_top_swipe_sum.y < min_px) return;
    if (s_top_swipe_sum.y < (LV_ABS(s_top_swipe_sum.x) + 10)) return;

    s_top_swipe_armed = false;
    lv_indev_stop_processing(indev);
    lv_indev_wait_release(indev);
    ui_go_to_page(UI_PAGE_SETTINGS, true);
}

static void settings_bottom_swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (!indev) return;

    if (s_transitioning) return;
    if (s_current_page != UI_PAGE_SETTINGS) return;

    if (code == LV_EVENT_PRESSED) {
        s_settings_swipe_sum.x = 0;
        s_settings_swipe_sum.y = 0;
        s_settings_swipe_armed = true;
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        s_settings_swipe_armed = false;
        return;
    }

    if (code != LV_EVENT_PRESSING) return;
    if (!s_settings_swipe_armed) return;

    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    s_settings_swipe_sum.x += v.x;
    s_settings_swipe_sum.y += v.y;

    const int32_t min_px = 30;
    if (s_settings_swipe_sum.y > -min_px) return;
    if (LV_ABS(s_settings_swipe_sum.y) < (LV_ABS(s_settings_swipe_sum.x) + 10)) return;

    s_settings_swipe_armed = false;
    lv_indev_stop_processing(indev);
    lv_indev_wait_release(indev);
    ui_go_to_page(UI_PAGE_DATA, true);
}

static void anim_set_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_settings_close_done(lv_anim_t *a)
{
    (void)a;
    if (s_page_settings) lv_obj_add_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
    if (s_top_gesture) {
        lv_obj_clear_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_top_gesture);
    }
    s_transitioning = false;
    ui_pages_relayout();
}

static void anim_settings_open_done(lv_anim_t *a)
{
    (void)a;
    s_transitioning = false;
    ui_pages_relayout();
}

static void ui_pages_relayout(void)
{
    if (!s_scr) return;

    if (s_page_data) {
        lv_obj_set_size(s_page_data, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(s_page_data, 0, 0);
    }

    lv_coord_t h = lv_obj_get_height(s_scr);
    if (s_page_settings) {
        lv_obj_set_size(s_page_settings, lv_pct(100), lv_pct(100));
        if (s_current_page == UI_PAGE_SETTINGS) {
            lv_obj_set_pos(s_page_settings, 0, 0);
            lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_pos(s_page_settings, 0, -h);
            lv_obj_add_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_top_gesture) {
        lv_obj_set_size(s_top_gesture, lv_pct(100), lv_pct(10));
        lv_obj_set_pos(s_top_gesture, 0, 0);
        if (s_current_page == UI_PAGE_DATA && !s_transitioning) lv_obj_clear_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_top_gesture);
    }

    if (s_settings_bottom_gesture) {
        lv_obj_set_size(s_settings_bottom_gesture, lv_pct(100), lv_pct(15));
        lv_obj_align(s_settings_bottom_gesture, LV_ALIGN_BOTTOM_MID, 0, 0);
        if (s_current_page == UI_PAGE_SETTINGS && !s_transitioning) lv_obj_clear_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_settings_bottom_gesture);
    }
}

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
    ui_pages_relayout();
    lvgl_port_unlock();

    /* data_page_set_orientation() locks internally */
    data_page_set_orientation(o);
}

void ui_go_to_page(ui_page_t page, bool animated)
{
    if (page >= UI_PAGE_COUNT) return;
    if (page == s_current_page) return;
    if (s_transitioning) return;

    lvgl_port_lock(0);

    if (!s_scr || !s_page_settings || !s_page_data) {
        lvgl_port_unlock();
        return;
    }

    lv_coord_t h = lv_obj_get_height(s_scr);

    if (!animated) {
        s_current_page = page;
        s_transitioning = false;
        ui_pages_relayout();
        lvgl_port_unlock();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page_settings);
    lv_anim_set_exec_cb(&a, anim_set_y);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);

    if (page == UI_PAGE_SETTINGS) {
        s_current_page = UI_PAGE_SETTINGS;
        s_transitioning = true;

        lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_page_settings, 0, -h);
        lv_obj_move_foreground(s_page_settings);
        lv_anim_set_values(&a, -h, 0);
        lv_anim_set_completed_cb(&a, anim_settings_open_done);

        if (s_top_gesture) lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_current_page = UI_PAGE_DATA;
        s_transitioning = true;

        lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_page_settings);
        lv_anim_set_values(&a, 0, -h);
        lv_anim_set_completed_cb(&a, anim_settings_close_done);

        if (s_top_gesture) lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_start(&a);
    lvgl_port_unlock();
}

static void create_pages_ui(void)
{
    s_scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(s_scr);
    ui_theme_apply_screen(s_scr);

    s_page_data = lv_obj_create(s_scr);
    lv_obj_set_size(s_page_data, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(s_page_data, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_page_data, LV_DIR_NONE);
    lv_obj_clear_flag(s_page_data, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_page_data, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_data, 0, 0);
    lv_obj_set_style_pad_all(s_page_data, 0, 0);
    data_page_create(s_page_data);

    s_page_settings = lv_obj_create(s_scr);
    lv_obj_set_size(s_page_settings, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollbar_mode(s_page_settings, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_page_settings, LV_DIR_NONE);
    lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_SCROLLABLE);
    ui_theme_apply_screen(s_page_settings);
    lv_obj_set_style_border_width(s_page_settings, 0, 0);
    lv_obj_set_style_pad_all(s_page_settings, 0, 0);
    settings_page_create(s_page_settings);

    s_current_page = UI_PAGE_DATA;

    s_top_gesture = lv_obj_create(s_scr);
    lv_obj_set_style_bg_opa(s_top_gesture, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_top_gesture, 0, 0);
    lv_obj_set_style_pad_all(s_top_gesture, 0, 0);
    lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_top_gesture, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_top_gesture, top_swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_top_gesture, top_swipe_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_top_gesture, top_swipe_event_cb, LV_EVENT_RELEASED, NULL);

    s_settings_bottom_gesture = lv_obj_create(s_page_settings);
    lv_obj_set_style_bg_opa(s_settings_bottom_gesture, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_settings_bottom_gesture, 0, 0);
    lv_obj_set_style_pad_all(s_settings_bottom_gesture, 0, 0);
    lv_obj_add_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_settings_bottom_gesture, settings_bottom_swipe_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_settings_bottom_gesture, settings_bottom_swipe_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_settings_bottom_gesture, settings_bottom_swipe_event_cb, LV_EVENT_RELEASED, NULL);

    ui_pages_relayout();
}

static void shutdown_btn_event_cb(lv_event_t *e)
{
    const char *tag = (const char *)lv_event_get_user_data(e);
    if (!tag) return;

    if (s_shutdown_overlay) {
        lv_obj_del(s_shutdown_overlay);
        s_shutdown_overlay = NULL;
    }

    if (strcmp(tag, "shutdown") == 0) {
        if (s_shutdown_confirm_cb) s_shutdown_confirm_cb();
    }
}

static void shutdown_prompt_create(void *unused)
{
    (void)unused;

    // If already open, don’t create twice
    if (s_shutdown_overlay) return;

    lv_obj_t *top = lv_layer_top();

    // Full-screen dim overlay
    s_shutdown_overlay = lv_obj_create(top);
    lv_obj_set_size(s_shutdown_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_shutdown_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_shutdown_overlay, 0, 0);
    lv_obj_clear_flag(s_shutdown_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // Center panel
    lv_obj_t *panel = lv_obj_create(s_shutdown_overlay);
    lv_obj_set_size(panel, 280, 180);
    lv_obj_center(panel);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Power Off?");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *msg = lv_label_create(panel);
    lv_label_set_text(msg, "Shut the device down now?");
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, 220);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 30);

    // Buttons row
    lv_obj_t *btn_cancel = lv_btn_create(panel);
    lv_obj_set_size(btn_cancel, 110, 45);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(btn_cancel, shutdown_btn_event_cb, LV_EVENT_CLICKED, (void*)"cancel");
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_center(lc);

    lv_obj_t *btn_shutdown = lv_btn_create(panel);
    lv_obj_set_size(btn_shutdown, 110, 45);
    lv_obj_align(btn_shutdown, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(btn_shutdown, shutdown_btn_event_cb, LV_EVENT_CLICKED, (void*)"shutdown");
    lv_obj_t *ls = lv_label_create(btn_shutdown);
    lv_label_set_text(ls, "Shutdown");
    lv_obj_center(ls);
}

void ui_show_shutdown_prompt(void)
{
    // Thread-safe: schedule on LVGL context
    lv_async_call(shutdown_prompt_create, NULL);
}


/* ---------- Public init ---------- */

void ui_init(lv_disp_t *disp)
{
    s_disp = disp;

    lvgl_port_lock(0);
    ui_theme_init(s_disp);
    create_pages_ui();
    ui_set_dark_mode(s_dark_mode);
    lvgl_port_unlock();
}
