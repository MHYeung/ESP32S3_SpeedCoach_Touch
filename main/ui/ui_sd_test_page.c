#include "ui_sd_test_page.h"
#include "lvgl.h"

static ui_sd_test_cb_t s_sd_test_cb = NULL;

void ui_register_sd_test_cb(ui_sd_test_cb_t cb)
{
    s_sd_test_cb = cb;
}

static void sd_btn_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_sd_test_cb)
    {
        s_sd_test_cb(); // call back into main.c: sd_test_button_action()
    }
}

void sd_test_page_create(lv_obj_t *parent)
{
    /* -------- "SD Test Tab" -------- */
    lv_obj_t *sd_label = lv_label_create(parent);
    lv_label_set_text(sd_label, "SD card test");
    lv_obj_align(sd_label, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *sd_btn = lv_button_create(parent);
    lv_obj_set_size(sd_btn, 160, 40);
    lv_obj_align(sd_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(sd_btn, sd_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sd_btn_label = lv_label_create(sd_btn);
    lv_label_set_text(sd_btn_label, "Write dummy CSV");
    lv_obj_center(sd_btn_label);
}