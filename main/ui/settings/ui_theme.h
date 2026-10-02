#pragma once

#include "lvgl.h"

#include <stdbool.h>

typedef struct {
    lv_color_t bg;
    lv_color_t surface;
    lv_color_t text;
    lv_color_t text_muted;
    lv_color_t border;
    lv_color_t accent;
    lv_color_t accent_text;
    lv_color_t work;
    lv_color_t rest;
    lv_color_t alert;
    lv_color_t rec;
} ui_theme_palette_t;

typedef enum {
    UI_THEME_LIGHT = 0,
    UI_THEME_DARK,
} ui_theme_t;

void ui_theme_init(lv_disp_t *disp);

void ui_theme_set(ui_theme_t theme);
ui_theme_t ui_theme_get(void);

/* Optional: override the built-in palette (call after ui_theme_init). */
void ui_theme_set_palette(const ui_theme_palette_t *palette);
const ui_theme_palette_t *ui_theme_palette(void);

/* Apply shared theme styles to objects (safe to call repeatedly). */
void ui_theme_apply_screen(lv_obj_t *screen);
void ui_theme_apply_surface(lv_obj_t *obj);
void ui_theme_apply_surface_border(lv_obj_t *obj);
void ui_theme_apply_label(lv_obj_t *label, bool muted);
void ui_theme_apply_button(lv_obj_t *btn);
void ui_theme_apply_button_secondary(lv_obj_t *btn);
void ui_theme_apply_stepper(lv_obj_t *btn);
void ui_theme_apply_field(lv_obj_t *obj);
void ui_theme_apply_switch(lv_obj_t *sw);
void ui_theme_apply_tile(lv_obj_t *obj);
/** One rounded card. Rows inside supply their own dividers. */
void ui_theme_apply_list_group(lv_obj_t *obj);

lv_color_t ui_theme_color_work(void);
lv_color_t ui_theme_color_rest(void);
lv_color_t ui_theme_color_alert(void);
lv_color_t ui_theme_color_rec(void);
lv_color_t ui_theme_color_accent(void);

