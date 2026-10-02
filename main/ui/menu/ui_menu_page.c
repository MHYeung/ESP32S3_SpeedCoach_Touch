// main/ui/ui_menu_page.c
#include "ui_menu_page.h"
#include "ui_activity_summary_page.h"
#include "ui_sensors_page.h"
#include "ui.h"
#include "ui_status_bar.h"
#include "ui_theme.h"
#include "esp_log.h"
#include <string.h>

#include "lvgl.h"

static const char *TAG = "ui_menu";

/* FontAwesome 6 Free Solid — custom subset (28 px, bpp 4) */
extern const lv_font_t lv_font_fa_solid_28;

/* UTF-8 encodings of the selected glyphs */
#define FA_ICON_MEDAL     "\xEF\x96\xA2"   /* U+F5A2  fa-medal       */
#define FA_ICON_STOPWATCH "\xEF\x8B\xB2"   /* U+F2F2  fa-stopwatch   */
#define FA_ICON_SLIDERS   "\xEF\x87\x9E"   /* U+F1DE  fa-sliders     */

static lv_obj_t *s_grid = NULL;

#define MENU_ICON_COUNT 6
static lv_obj_t *s_btns[MENU_ICON_COUNT] = {0};
static uint8_t s_btn_count = 0;

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_status;

static void activity_summary_refresh_async(void *p)
{
    (void)p;
    activity_summary_page_refresh();
}

static void menu_icon_cb(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!id)
        return;

    if (strcmp(id, "settings") == 0) {
        ui_go_to_page(UI_SETTINGS_PAGE, true);
        return;
    }
    if (strcmp(id, "activity_summary") == 0) {
        ui_go_to_page(UI_ACTIVITY_SUMMARY_PAGE, true);
        lv_async_call(activity_summary_refresh_async, NULL);
        return;
    }
    if (strcmp(id, "interval") == 0) {
        ui_defer_go_to_page(UI_INTERVAL_SETUP_PAGE);
        return;
    }
    if (strcmp(id, "race") == 0) {
        ui_defer_go_to_page(UI_RACE_SETUP_PAGE);
        return;
    }
    if (strcmp(id, "step") == 0) {
        ui_defer_go_to_page(UI_STEP_SETUP_PAGE);
        return;
    }
    if (strcmp(id, "sensors") == 0) {
        sensors_page_set_return_page(UI_PAGE_MENU);
        ui_go_to_page(UI_SENSORS_PAGE, true);
        return;
    }

    ESP_LOGI(TAG, "Menu icon pressed: %s", id);
}

static void theme_tile_children(lv_obj_t *tile)
{
    if (!tile)
        return;
    lv_obj_t *ic = lv_obj_get_child(tile, 0);
    lv_obj_t *lb = lv_obj_get_child(tile, 1);
    if (ic) {
        ui_theme_apply_label(ic, false);
        lv_obj_set_style_text_color(ic, ui_theme_color_accent(), 0);
    }
    if (lb)
        ui_theme_apply_label(lb, false);
}

static lv_obj_t *create_icon_btn(lv_obj_t *parent, const char *symbol, const char *text,
                                 const char *id, lv_coord_t btn_size, const lv_font_t *icon_font)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    ui_theme_apply_tile(btn);
    lv_obj_set_size(btn, btn_size, btn_size);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, menu_icon_cb, LV_EVENT_CLICKED, (void *)id);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_set_style_pad_row(btn, 2, 0);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    ui_theme_apply_label(ic, false);
    lv_obj_set_style_text_font(ic, icon_font ? icon_font : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ic, ui_theme_color_accent(), 0);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    ui_theme_apply_label(lb, false);
    lv_obj_set_width(lb, btn_size);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);

    return btn;
}

static void menu_apply_grid_layout(void)
{
    if (!s_grid)
        return;

    const bool land = ui_is_landscape();
    const int cols = land ? 3 : 2;
    const int rows = land ? 2 : 3;

    lv_obj_set_layout(s_grid, LV_LAYOUT_GRID);

    static lv_coord_t col_2[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t col_3[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_2[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_3[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    lv_obj_set_grid_dsc_array(s_grid, cols == 3 ? col_3 : col_2, rows == 3 ? row_3 : row_2);

    lv_obj_update_layout(s_grid);
    lv_coord_t cw = lv_obj_get_content_width(s_grid);
    lv_coord_t ch = lv_obj_get_content_height(lv_obj_get_parent(s_grid));
    if (ch <= 0)
        ch = lv_obj_get_content_height(s_grid);
    lv_coord_t gapc = lv_obj_get_style_pad_column(s_grid, 0);
    lv_coord_t gapr = lv_obj_get_style_pad_row(s_grid, 0);

    lv_coord_t btn_w = (cw - gapc * (cols - 1)) / cols;
    lv_coord_t btn_h = (ch - gapr * (rows - 1)) / rows;
    if (btn_w < 48) {
        btn_w = 48;
    }
    if (btn_h < 48) {
        btn_h = 48;
    }

    for (uint8_t i = 0; i < s_btn_count; i++) {
        lv_obj_set_size(s_btns[i], btn_w, btn_h);

        int c = i % cols;
        int r = i / cols;
        lv_obj_set_grid_cell(
            s_btns[i],
            LV_GRID_ALIGN_CENTER, c, 1,
            LV_GRID_ALIGN_CENTER, r, 1);

        lv_obj_t *lb = lv_obj_get_child(s_btns[i], 1);
        if (lb)
            lv_obj_set_width(lb, btn_w);
    }

    lv_obj_set_height(s_grid, rows * btn_h + gapr * (rows - 1));
}

void menu_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);

    ui_status_bar_create(&s_status, s_root);

    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 6, 0);

    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_grid = lv_obj_create(body);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_style_pad_row(s_grid, 8, 0);
    lv_obj_set_style_pad_column(s_grid, 8, 0);

    lv_coord_t btn_size = 72;

    s_btn_count = 0;
    s_btns[s_btn_count++] = create_icon_btn(s_grid, FA_ICON_MEDAL, "Activity", "activity_summary",
                                            btn_size, &lv_font_fa_solid_28);
    s_btns[s_btn_count++] = create_icon_btn(s_grid, FA_ICON_STOPWATCH, "Interval", "interval",
                                            btn_size, &lv_font_fa_solid_28);
    s_btns[s_btn_count++] = create_icon_btn(s_grid, LV_SYMBOL_PLAY, "Race", "race",
                                            btn_size, &lv_font_montserrat_28);
    s_btns[s_btn_count++] = create_icon_btn(s_grid, LV_SYMBOL_UP, "Step Test", "step",
                                            btn_size, &lv_font_montserrat_28);
    s_btns[s_btn_count++] = create_icon_btn(s_grid, LV_SYMBOL_BLUETOOTH, "Sensors", "sensors",
                                            btn_size, &lv_font_montserrat_28);
    s_btns[s_btn_count++] = create_icon_btn(s_grid, FA_ICON_SLIDERS, "Settings", "settings",
                                            btn_size, &lv_font_fa_solid_28);

    menu_apply_grid_layout();
}

void menu_page_apply_theme(void)
{
    if (!s_root)
        return;
    ui_status_bar_apply_theme(&s_status);
    for (uint8_t i = 0; i < s_btn_count; i++) {
        if (!s_btns[i])
            continue;
        ui_theme_apply_tile(s_btns[i]);
        theme_tile_children(s_btns[i]);
    }
}

void menu_page_on_orientation_changed(void)
{
    if (!s_root)
        return;
    ui_status_bar_force_refresh(&s_status);
    menu_apply_grid_layout();
}
