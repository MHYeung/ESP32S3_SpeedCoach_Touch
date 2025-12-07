
#pragma once
#include "lvgl.h"

typedef void (*ui_sd_test_cb_t)(void);
void ui_register_sd_test_cb(ui_sd_test_cb_t cb);
void sd_test_page_create(lv_obj_t *parent);
