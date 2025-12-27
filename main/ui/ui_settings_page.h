// main/ui/ui_settings_page.h
#pragma once
#include "lvgl.h"

/* Build Settings tab: dark-mode + auto-rotate rows */
void settings_page_create(lv_obj_t *parent);
void settings_page_apply_theme(void);

/* Status bar updates (dummy sources for now). */
void settings_page_set_gps_status(bool connected, uint8_t bars_0_to_4);
void settings_page_set_dark_mode_state(bool enabled);
void settings_page_on_orientation_changed(void);

typedef void (*ui_split_length_cb_t)(uint32_t length_m);
/* Register a callback to handle backend CSV updates */
void ui_settings_register_split_length_cb(ui_split_length_cb_t cb);