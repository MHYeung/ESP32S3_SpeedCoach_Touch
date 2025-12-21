#pragma once

#include "lvgl.h"
#include <stdbool.h>

void ui_init(lv_disp_t *disp);

typedef enum {
    UI_ORIENT_PORTRAIT_0,
    UI_ORIENT_LANDSCAPE_90,
    UI_ORIENT_PORTRAIT_180,
    UI_ORIENT_LANDSCAPE_270,
} ui_orientation_t;

typedef enum {
    UI_PAGE_DATA = 0,
    UI_PAGE_MENU = 1,
    UI_ACTIVITY_SUMMARY_PAGE = 2,
    UI_PAGE_SETTINGS = 3,
    UI_PAGE_COUNT,
} ui_page_t;

void ui_set_orientation(ui_orientation_t o);
void ui_go_to_page(ui_page_t page, bool animated);

/* Theme helpers (currently implemented as light/dark). */
void ui_set_dark_mode(bool enabled);
bool ui_get_dark_mode(void);

typedef void (*ui_dark_mode_cb_t)(bool enabled);
typedef void (*ui_auto_rotate_cb_t)(bool enabled);

void ui_register_dark_mode_cb(ui_dark_mode_cb_t cb);
void ui_register_auto_rotate_cb(ui_auto_rotate_cb_t cb);

/* NEW: internal helpers that per-page code can call */
void ui_notify_dark_mode_changed(bool enabled);
void ui_notify_auto_rotate_changed(bool enabled);
