// main/ui/ui_controls_page.c
#include "ui_controls_page.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static void btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn   = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    const char *txt = lv_label_get_text(label);
    if (strcmp(txt, "Click me") == 0) {
        lv_label_set_text(label, "Touched!");
    } else {
        lv_label_set_text(label, "Click me");
    }
}
/* We keep the label in a static so the event callback can update it */
static lv_obj_t *s_slider_label = NULL;

/* Event callback for the slider */
static void slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);

    if (!s_slider_label) {
        return;
    }

    int32_t v = lv_slider_get_value(slider);

    char buf[32];
    snprintf(buf, sizeof(buf), "Value: %ld", (long)v);
    lv_label_set_text(s_slider_label, buf);
}

void controls_page_create(lv_obj_t *parent)
{
    /* -------- Controls Tab -------- */
/* Create a full-page container with vertical (column) layout */
lv_obj_t *ctrl_cont = lv_obj_create(parent);
lv_obj_set_size(ctrl_cont, lv_pct(100), lv_pct(100));

/* Enable vertical scrolling on this page */
lv_obj_set_scroll_dir(ctrl_cont, LV_DIR_VER);
lv_obj_set_scrollbar_mode(ctrl_cont, LV_SCROLLBAR_MODE_AUTO);

/* Flex: vertical stack, horizontally centered */
lv_obj_set_flex_flow(ctrl_cont, LV_FLEX_FLOW_COLUMN);
lv_obj_set_flex_align(ctrl_cont,
                      LV_FLEX_ALIGN_START,   // main axis (top→bottom)
                      LV_FLEX_ALIGN_CENTER,  // cross axis (center horizontally)
                      LV_FLEX_ALIGN_CENTER); // track alignment

lv_obj_set_style_pad_all(ctrl_cont, 8, 0);
lv_obj_set_style_pad_row(ctrl_cont, 6, 0);
lv_obj_set_style_border_width(ctrl_cont, 0, 0);

/* 1) Title label – full width, centered text */
lv_obj_t *label1 = lv_label_create(ctrl_cont);
lv_obj_set_width(label1, lv_pct(100));                    // span container width
lv_obj_set_style_text_align(label1, LV_TEXT_ALIGN_CENTER, 0);
lv_label_set_text(label1, "Controls page");

/* 2) Button – centered by flex */
lv_obj_t *btn = lv_button_create(ctrl_cont);
lv_obj_set_size(btn, 120, 40);
lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

lv_obj_t *btn_label = lv_label_create(btn);
lv_label_set_text(btn_label, "Click me");
lv_obj_center(btn_label);

/* 3) Slider */
lv_obj_t *slider = lv_slider_create(ctrl_cont);
lv_obj_set_width(slider, lv_pct(80));   // 80% of container width
lv_obj_set_style_pad_all(slider, 8, 0);
lv_obj_add_event_cb(slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

/* 4) Slider value label */
s_slider_label = lv_label_create(ctrl_cont);
lv_label_set_text(s_slider_label, "Slider: 0");
}
