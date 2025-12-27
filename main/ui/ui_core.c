// main/ui/ui_core.c
#include "ui.h"
#include "ui_data_page.h"
#include "ui_settings_page.h"
#include "ui_theme.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <string.h>
#include "esp_log.h"

/* Global LVGL handles for the UI core */
static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_page_data = NULL;
static lv_obj_t *s_page_settings = NULL;

/* Gesture layers */
static lv_obj_t *s_top_gesture = NULL;
static lv_obj_t *s_settings_bottom_gesture = NULL;
static ui_page_t s_current_page = UI_PAGE_DATA;
static bool s_transitioning = false;

/* Gesture State */
static bool s_top_swipe_armed = false;
static lv_point_t s_top_swipe_sum = {0};
static bool s_settings_swipe_armed = false;
static lv_point_t s_settings_swipe_sum = {0};

/* Shutdown Dialog Handles */
static ui_shutdown_confirm_cb_t s_shutdown_confirm_cb = NULL;
static lv_obj_t *s_shutdown_overlay = NULL;
static lv_obj_t *s_shutdown_panel = NULL;
static lv_obj_t *s_shutdown_btn_box = NULL;
static lv_obj_t *s_shutdown_msg = NULL;
static lv_obj_t *s_btn_shutdown = NULL;
static lv_obj_t *s_btn_shutdown_cancel = NULL;

/* Stop/Save Dialog Handles */
static ui_stop_save_confirm_cb_t s_stop_save_confirm_cb = NULL;
static lv_obj_t *s_stop_save_overlay = NULL;
static lv_obj_t *s_stop_save_panel = NULL;
static lv_obj_t *s_stop_save_btn_box = NULL;
static lv_obj_t *s_stop_save_msg = NULL;
static lv_obj_t *s_btn_save = NULL;
static lv_obj_t *s_btn_save_cancel = NULL;

/* Callbacks registered from main.c */
static ui_dark_mode_cb_t s_dark_mode_cb = NULL;
static ui_auto_rotate_cb_t s_auto_rotate_cb = NULL;
static bool s_dark_mode = true;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static bool ui_is_landscape(void)
{
    if (!s_disp) return false;
    lv_display_rotation_t r = lv_display_get_rotation(s_disp);
    return (r == LV_DISPLAY_ROTATION_90 || r == LV_DISPLAY_ROTATION_270);
}

static void ui_pages_relayout(void);
static void ui_relayout_dialogs(void);

/* -------------------------------------------------------------------------- */
/* Callback Registration                                                     */
/* -------------------------------------------------------------------------- */

void ui_register_stop_save_confirm_cb(ui_stop_save_confirm_cb_t cb) { s_stop_save_confirm_cb = cb; }
void ui_register_shutdown_confirm_cb(ui_shutdown_confirm_cb_t cb) { s_shutdown_confirm_cb = cb; }
void ui_register_dark_mode_cb(ui_dark_mode_cb_t cb) { s_dark_mode_cb = cb; }
void ui_register_auto_rotate_cb(ui_auto_rotate_cb_t cb) { s_auto_rotate_cb = cb; }

void ui_notify_dark_mode_changed(bool enabled)
{
    ui_set_dark_mode(enabled);
    if (s_dark_mode_cb) s_dark_mode_cb(enabled);
}

void ui_notify_auto_rotate_changed(bool enabled)
{
    if (s_auto_rotate_cb) s_auto_rotate_cb(enabled);
}

void ui_set_dark_mode(bool enabled)
{
    s_dark_mode = enabled;
    lvgl_port_lock(0);
    ui_theme_set(enabled ? UI_THEME_DARK : UI_THEME_LIGHT);
    data_page_apply_theme();
    settings_page_apply_theme();
    lvgl_port_unlock();
}

bool ui_get_dark_mode(void) { return s_dark_mode; }

/* -------------------------------------------------------------------------- */
/* Gesture Handling                                                          */
/* -------------------------------------------------------------------------- */

static void top_swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (!indev || s_transitioning || s_current_page != UI_PAGE_DATA) return;

    if (code == LV_EVENT_PRESSED) {
        s_top_swipe_sum.x = 0; s_top_swipe_sum.y = 0; s_top_swipe_armed = true;
    } else if (code == LV_EVENT_RELEASED) {
        s_top_swipe_armed = false;
    } else if (code == LV_EVENT_PRESSING && s_top_swipe_armed) {
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        s_top_swipe_sum.x += v.x;
        s_top_swipe_sum.y += v.y;

        // Down swipe detected
        if (s_top_swipe_sum.y > 30 && s_top_swipe_sum.y > (LV_ABS(s_top_swipe_sum.x) + 10)) {
            s_top_swipe_armed = false;
            lv_indev_stop_processing(indev);
            lv_indev_wait_release(indev);
            ui_go_to_page(UI_PAGE_SETTINGS, true);
        }
    }
}

static void settings_bottom_swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_param(e);
    if (!indev || s_transitioning || s_current_page != UI_PAGE_SETTINGS) return;

    if (code == LV_EVENT_PRESSED) {
        s_settings_swipe_sum.x = 0; s_settings_swipe_sum.y = 0; s_settings_swipe_armed = true;
    } else if (code == LV_EVENT_RELEASED) {
        s_settings_swipe_armed = false;
    } else if (code == LV_EVENT_PRESSING && s_settings_swipe_armed) {
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        s_settings_swipe_sum.x += v.x;
        s_settings_swipe_sum.y += v.y;

        // Up swipe detected
        if (s_settings_swipe_sum.y < -30 && LV_ABS(s_settings_swipe_sum.y) > (LV_ABS(s_settings_swipe_sum.x) + 10)) {
            s_settings_swipe_armed = false;
            lv_indev_stop_processing(indev);
            lv_indev_wait_release(indev);
            ui_go_to_page(UI_PAGE_DATA, true);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Page Animations & Layout                                                  */
/* -------------------------------------------------------------------------- */

static void anim_set_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v); }
static void anim_done_cb(lv_anim_t *a) { s_transitioning = false; ui_pages_relayout(); }

static void ui_pages_relayout(void)
{
    if (!s_scr) return;
    lv_coord_t h = lv_obj_get_height(s_scr);

    if (s_page_data) {
        lv_obj_set_size(s_page_data, lv_pct(100), lv_pct(100));
        lv_obj_set_pos(s_page_data, 0, 0);
    }

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
        lv_obj_set_size(s_top_gesture, lv_pct(100), lv_pct(15));
        lv_obj_set_pos(s_top_gesture, 0, 0);
        if (s_current_page == UI_PAGE_DATA && !s_transitioning) {
            lv_obj_clear_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_top_gesture);
        } else {
            lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_settings_bottom_gesture) {
        lv_obj_set_size(s_settings_bottom_gesture, lv_pct(100), lv_pct(15));
        lv_obj_align(s_settings_bottom_gesture, LV_ALIGN_BOTTOM_MID, 0, 0);
        if (s_current_page == UI_PAGE_SETTINGS && !s_transitioning) {
            lv_obj_clear_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_settings_bottom_gesture);
        } else {
            lv_obj_add_flag(s_settings_bottom_gesture, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_set_orientation(ui_orientation_t o)
{
    lv_display_rotation_t rot = LV_DISPLAY_ROTATION_0;
    switch (o) {
        case UI_ORIENT_PORTRAIT_0: rot = LV_DISPLAY_ROTATION_0; break;
        case UI_ORIENT_LANDSCAPE_90: rot = LV_DISPLAY_ROTATION_90; break;
        case UI_ORIENT_PORTRAIT_180: rot = LV_DISPLAY_ROTATION_180; break;
        case UI_ORIENT_LANDSCAPE_270: rot = LV_DISPLAY_ROTATION_270; break;
    }

    lvgl_port_lock(0);
    lv_display_set_rotation(s_disp, rot);
    
    // Updates
    ui_pages_relayout();
    ui_relayout_dialogs(); // <--- FIX: Resize dialogs immediately
    settings_page_on_orientation_changed();
    
    lvgl_port_unlock();

    data_page_set_orientation(o);
}

void ui_go_to_page(ui_page_t page, bool animated)
{
    if (page == s_current_page || s_transitioning || !s_page_settings) return;

    lvgl_port_lock(0);
    lv_coord_t h = lv_obj_get_height(s_scr);

    if (!animated) {
        s_current_page = page;
        ui_pages_relayout();
        lvgl_port_unlock();
        return;
    }

    s_transitioning = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page_settings);
    lv_anim_set_exec_cb(&a, anim_set_y);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, anim_done_cb);

    if (page == UI_PAGE_SETTINGS) {
        s_current_page = UI_PAGE_SETTINGS;
        lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_page_settings);
        lv_anim_set_values(&a, -h, 0);
        if(s_top_gesture) lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_current_page = UI_PAGE_DATA;
        lv_anim_set_values(&a, 0, -h);
        if(s_top_gesture) lv_obj_add_flag(s_top_gesture, LV_OBJ_FLAG_HIDDEN);
    }

    lv_anim_start(&a);
    lvgl_port_unlock();
}

/* -------------------------------------------------------------------------- */
/* Common Dialog Layout Logic                                                */
/* -------------------------------------------------------------------------- */

// Generic helper to style the dialog based on orientation
static void style_dialog_panel(lv_obj_t *panel, lv_obj_t *btn_box, lv_obj_t *btn1, lv_obj_t *btn2, lv_obj_t *msg_lbl)
{
    if (!panel) return;
    
    bool land = ui_is_landscape();
    lv_coord_t w_panel = land ? 280 : 220; // Default width
    
    lv_obj_set_width(panel, w_panel);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);

    // Adjust padding: Less padding in landscape to save vertical space
    lv_obj_set_style_pad_all(panel, land ? 10 : 14, 0);
    lv_obj_set_style_pad_row(panel, land ? 4 : 14, 0);

    if (msg_lbl) {
        lv_obj_set_width(msg_lbl, lv_pct(100));
        lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    }

    if (btn_box) {
        lv_obj_set_width(btn_box, lv_pct(100));
        lv_obj_set_height(btn_box, LV_SIZE_CONTENT);
        
        if (land) {
            // Row Layout
            lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btn_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(btn_box, 4, 0);
            
            // Percentage width ensures they fit regardless of panel width
            // 47% + 47% + ~6% gap
            if (btn1) lv_obj_set_width(btn1, lv_pct(47));
            if (btn2) lv_obj_set_width(btn2, lv_pct(47));
        } else {
            // Column Layout
            lv_obj_set_flex_flow(btn_box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(btn_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(btn_box, 10, 0);
            
            if (btn1) lv_obj_set_width(btn1, lv_pct(100));
            if (btn2) lv_obj_set_width(btn2, lv_pct(100));
        }
    }
}

static void ui_relayout_dialogs(void)
{
    if (s_shutdown_overlay) {
        style_dialog_panel(s_shutdown_panel, s_shutdown_btn_box, s_btn_shutdown, s_btn_shutdown_cancel, s_shutdown_msg);
    }
    if (s_stop_save_overlay) {
        style_dialog_panel(s_stop_save_panel, s_stop_save_btn_box, s_btn_save, s_btn_save_cancel, s_stop_save_msg);
    }
}

/* -------------------------------------------------------------------------- */
/* Shutdown Prompt                                                           */
/* -------------------------------------------------------------------------- */

static void shutdown_btn_event_cb(lv_event_t *e)
{
    const char *tag = (const char *)lv_event_get_user_data(e);
    if (!tag) return;

    if (s_shutdown_overlay) {
        lv_obj_del(s_shutdown_overlay);
        s_shutdown_overlay = NULL;
        s_shutdown_panel = NULL;
        s_shutdown_btn_box = NULL;
    }

    if (strcmp(tag, "shutdown") == 0) {
        if (s_shutdown_confirm_cb) s_shutdown_confirm_cb();
    }
}

static void shutdown_prompt_create(void *unused)
{
    (void)unused;
    if (s_shutdown_overlay) return;

    lv_obj_t *top = lv_layer_top();

    s_shutdown_overlay = lv_obj_create(top);
    lv_obj_set_size(s_shutdown_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_shutdown_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_shutdown_overlay, 0, 0);
    lv_obj_set_flex_flow(s_shutdown_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_shutdown_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_shutdown_panel = lv_obj_create(s_shutdown_overlay);
    ui_theme_apply_surface(s_shutdown_panel);
    // Initial padding (will be updated by relayout immediately)
    lv_obj_set_style_pad_all(s_shutdown_panel, 14, 0);
    lv_obj_set_style_pad_row(s_shutdown_panel, 14, 0);
    lv_obj_set_flex_flow(s_shutdown_panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_shutdown_panel);
    lv_label_set_text(title, "Power Off?");
    ui_theme_apply_label(title, false);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    s_shutdown_msg = lv_label_create(s_shutdown_panel);
    lv_label_set_text(s_shutdown_msg, "Shut the device down now?");
    ui_theme_apply_label(s_shutdown_msg, true);
    lv_obj_set_style_text_align(s_shutdown_msg, LV_TEXT_ALIGN_CENTER, 0);

    // Button Container
    s_shutdown_btn_box = lv_obj_create(s_shutdown_panel);
    lv_obj_set_style_bg_opa(s_shutdown_btn_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_shutdown_btn_box, 0, 0);
    lv_obj_set_style_pad_all(s_shutdown_btn_box, 0, 0);

    // Buttons
    s_btn_shutdown = lv_btn_create(s_shutdown_btn_box);
    lv_obj_add_event_cb(s_btn_shutdown, shutdown_btn_event_cb, LV_EVENT_CLICKED, (void *)"shutdown");
    lv_obj_set_style_bg_color(s_btn_shutdown, lv_color_hex(0xEF4444), 0); // Red
    lv_obj_t *l1 = lv_label_create(s_btn_shutdown);
    lv_label_set_text(l1, "Shutdown");
    lv_obj_center(l1);

    s_btn_shutdown_cancel = lv_btn_create(s_shutdown_btn_box);
    lv_obj_add_event_cb(s_btn_shutdown_cancel, shutdown_btn_event_cb, LV_EVENT_CLICKED, (void *)"cancel");
    lv_obj_set_style_bg_color(s_btn_shutdown_cancel, lv_color_hex(0x6B7280), 0); // Grey
    lv_obj_t *l2 = lv_label_create(s_btn_shutdown_cancel);
    lv_label_set_text(l2, "Cancel");
    lv_obj_center(l2);

    // Initial Layout
    ui_relayout_dialogs();
}

void ui_show_shutdown_prompt(void)
{
    lv_async_call(shutdown_prompt_create, NULL);
}

/* -------------------------------------------------------------------------- */
/* Stop/Save Prompt                                                          */
/* -------------------------------------------------------------------------- */

static void stop_save_btn_event_cb(lv_event_t *e)
{
    const char *tag = (const char *)lv_event_get_user_data(e);
    if (!tag) return;

    if (s_stop_save_overlay) {
        lv_obj_del(s_stop_save_overlay);
        s_stop_save_overlay = NULL;
        s_stop_save_panel = NULL;
        s_stop_save_btn_box = NULL;
    }

    if (strcmp(tag, "stop_save") == 0) {
        if (s_stop_save_confirm_cb) s_stop_save_confirm_cb();
    }
}

static void stop_save_prompt_create(void *unused)
{
    (void)unused;
    if (s_stop_save_overlay) return;

    lv_obj_t *top = lv_layer_top();

    s_stop_save_overlay = lv_obj_create(top);
    lv_obj_set_size(s_stop_save_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_stop_save_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_stop_save_overlay, 0, 0);
    lv_obj_set_flex_flow(s_stop_save_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_stop_save_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_stop_save_panel = lv_obj_create(s_stop_save_overlay);
    ui_theme_apply_surface(s_stop_save_panel);
    // Initial padding
    lv_obj_set_style_pad_all(s_stop_save_panel, 14, 0);
    lv_obj_set_style_pad_row(s_stop_save_panel, 14, 0);
    lv_obj_set_flex_flow(s_stop_save_panel, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_stop_save_panel);
    lv_label_set_text(title, "Stop Activity?");
    ui_theme_apply_label(title, false);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, lv_pct(100));

    s_stop_save_msg = lv_label_create(s_stop_save_panel);
    lv_label_set_text(s_stop_save_msg, "Stop and save this session?");
    ui_theme_apply_label(s_stop_save_msg, true);
    lv_obj_set_style_text_align(s_stop_save_msg, LV_TEXT_ALIGN_CENTER, 0);

    // Button Container
    s_stop_save_btn_box = lv_obj_create(s_stop_save_panel);
    lv_obj_set_style_bg_opa(s_stop_save_btn_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_stop_save_btn_box, 0, 0);
    lv_obj_set_style_pad_all(s_stop_save_btn_box, 0, 0);

    // Buttons
    s_btn_save = lv_btn_create(s_stop_save_btn_box);
    lv_obj_add_event_cb(s_btn_save, stop_save_btn_event_cb, LV_EVENT_CLICKED, (void *)"stop_save");
    ui_theme_apply_button(s_btn_save);
    lv_obj_t *l1 = lv_label_create(s_btn_save);
    lv_label_set_text(l1, "Save");
    lv_obj_center(l1);

    s_btn_save_cancel = lv_btn_create(s_stop_save_btn_box);
    lv_obj_add_event_cb(s_btn_save_cancel, stop_save_btn_event_cb, LV_EVENT_CLICKED, (void *)"cancel");
    lv_obj_set_style_bg_color(s_btn_save_cancel, lv_color_hex(0x6B7280), 0);
    lv_obj_t *l2 = lv_label_create(s_btn_save_cancel);
    lv_label_set_text(l2, "Cancel");
    lv_obj_center(l2);

    // Initial Layout
    ui_relayout_dialogs();
}

void ui_show_stop_save_prompt(void)
{
    lv_async_call(stop_save_prompt_create, NULL);
}

/* -------------------------------------------------------------------------- */
/* Init                                                                      */
/* -------------------------------------------------------------------------- */

static void create_pages_ui(void)
{
    s_scr = lv_disp_get_scr_act(s_disp);
    lv_obj_clean(s_scr);
    ui_theme_apply_screen(s_scr);

    // 1. Data Page
    s_page_data = lv_obj_create(s_scr);
    lv_obj_set_size(s_page_data, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_page_data, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_page_data, 0, 0);
    lv_obj_set_style_bg_opa(s_page_data, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_data, 0, 0);
    data_page_create(s_page_data);

    // 2. Settings Page
    s_page_settings = lv_obj_create(s_scr);
    lv_obj_set_size(s_page_settings, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_page_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_page_settings, 0, 0);
    lv_obj_set_style_border_width(s_page_settings, 0, 0);
    ui_theme_apply_screen(s_page_settings);
    settings_page_create(s_page_settings);

    // 3. Gestures
    s_top_gesture = lv_obj_create(s_scr);
    lv_obj_set_style_bg_opa(s_top_gesture, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_top_gesture, 0, 0);
    lv_obj_add_event_cb(s_top_gesture, top_swipe_event_cb, LV_EVENT_ALL, NULL);

    s_settings_bottom_gesture = lv_obj_create(s_page_settings);
    lv_obj_set_style_bg_opa(s_settings_bottom_gesture, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_settings_bottom_gesture, 0, 0);
    lv_obj_add_event_cb(s_settings_bottom_gesture, settings_bottom_swipe_event_cb, LV_EVENT_ALL, NULL);

    s_current_page = UI_PAGE_DATA;
    ui_pages_relayout();
}

void ui_init(lv_disp_t *disp)
{
    s_disp = disp;
    lvgl_port_lock(0);
    ui_theme_init(s_disp);
    create_pages_ui();
    lvgl_port_unlock();
}