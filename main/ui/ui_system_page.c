// main/ui/ui_system_page.c
#include "ui_system_page.h"

void system_page_create(lv_obj_t *parent)
{
    lv_obj_t *sys_label = lv_label_create(parent);
    lv_label_set_text(sys_label, "System info (placeholder)");
    lv_obj_align(sys_label, LV_ALIGN_TOP_LEFT, 4, 4);
}
