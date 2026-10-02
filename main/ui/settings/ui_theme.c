#include "ui_theme.h"

static lv_disp_t *s_disp = NULL;
static ui_theme_t s_theme = UI_THEME_LIGHT;
static ui_theme_palette_t s_palette;

static bool s_styles_inited = false;
static lv_style_t s_style_screen;
static lv_style_t s_style_surface;
static lv_style_t s_style_surface_border;
static lv_style_t s_style_text;
static lv_style_t s_style_text_muted;
static lv_style_t s_style_button;
static lv_style_t s_style_button_pressed;
static lv_style_t s_style_button_sec;
static lv_style_t s_style_button_sec_pressed;
static lv_style_t s_style_switch_track;
static lv_style_t s_style_switch_track_checked;
static lv_style_t s_style_switch_knob;
static lv_style_t s_style_tile;
static lv_style_t s_style_tile_pressed;

static ui_theme_palette_t palette_light(void)
{
    return (ui_theme_palette_t){
        .bg = lv_color_hex(0xEEF1F5),
        .surface = lv_color_hex(0xFFFFFF),
        .text = lv_color_hex(0x1A2433),
        .text_muted = lv_color_hex(0x2C3A4B),
        .border = lv_color_hex(0xD5DCE6),
        .accent = lv_color_hex(0x2B5EA7),
        .accent_text = lv_color_hex(0xFFFFFF),
        .work = lv_color_hex(0xEA580C),
        .rest = lv_color_hex(0x16A34A),
        .alert = lv_color_hex(0xDC2626),
        .rec = lv_color_hex(0xEF4444),
    };
}

static ui_theme_palette_t palette_dark(void)
{
    return (ui_theme_palette_t){
        .bg = lv_color_hex(0x101826),
        .surface = lv_color_hex(0x1B2434),
        .text = lv_color_hex(0xE8EEF6),
        .text_muted = lv_color_hex(0x8B9BB0),
        .border = lv_color_hex(0x334155),
        .accent = lv_color_hex(0x7BA3D9),
        .accent_text = lv_color_hex(0x101826),
        .work = lv_color_hex(0xFB923C),
        .rest = lv_color_hex(0x4ADE80),
        .alert = lv_color_hex(0xF87171),
        .rec = lv_color_hex(0xF87171),
    };
}

static void styles_init_once(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    lv_style_init(&s_style_screen);
    lv_style_init(&s_style_surface);
    lv_style_init(&s_style_surface_border);
    lv_style_init(&s_style_text);
    lv_style_init(&s_style_text_muted);
    lv_style_init(&s_style_button);
    lv_style_init(&s_style_button_pressed);
    lv_style_init(&s_style_button_sec);
    lv_style_init(&s_style_button_sec_pressed);
    lv_style_init(&s_style_switch_track);
    lv_style_init(&s_style_switch_track_checked);
    lv_style_init(&s_style_switch_knob);
    lv_style_init(&s_style_tile);
    lv_style_init(&s_style_tile_pressed);

    lv_style_set_bg_opa(&s_style_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&s_style_screen, 0);
    lv_style_set_border_width(&s_style_screen, 0);

    lv_style_set_bg_opa(&s_style_surface, LV_OPA_COVER);
    lv_style_set_radius(&s_style_surface, 12);
    lv_style_set_pad_all(&s_style_surface, 8);
    lv_style_set_border_width(&s_style_surface, 0);

    lv_style_set_bg_opa(&s_style_surface_border, LV_OPA_COVER);
    lv_style_set_radius(&s_style_surface_border, 12);
    lv_style_set_pad_all(&s_style_surface_border, 8);
    lv_style_set_border_width(&s_style_surface_border, 1);

    lv_style_set_text_opa(&s_style_text, LV_OPA_COVER);
    lv_style_set_text_opa(&s_style_text_muted, LV_OPA_COVER);

    lv_style_set_radius(&s_style_button, 8);
    lv_style_set_bg_opa(&s_style_button, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button, 0);
    lv_style_set_pad_hor(&s_style_button, 10);
    lv_style_set_pad_ver(&s_style_button, 6);
    lv_style_set_min_height(&s_style_button, 36);

    lv_style_set_radius(&s_style_button_pressed, 8);
    lv_style_set_bg_opa(&s_style_button_pressed, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button_pressed, 0);
    lv_style_set_pad_hor(&s_style_button_pressed, 10);
    lv_style_set_pad_ver(&s_style_button_pressed, 6);
    lv_style_set_opa(&s_style_button_pressed, LV_OPA_COVER);

    lv_style_set_radius(&s_style_button_sec, 8);
    lv_style_set_bg_opa(&s_style_button_sec, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button_sec, 1);
    lv_style_set_pad_hor(&s_style_button_sec, 10);
    lv_style_set_pad_ver(&s_style_button_sec, 6);
    lv_style_set_min_height(&s_style_button_sec, 36);

    lv_style_set_radius(&s_style_button_sec_pressed, 8);
    lv_style_set_bg_opa(&s_style_button_sec_pressed, LV_OPA_COVER);
    lv_style_set_border_width(&s_style_button_sec_pressed, 1);
    lv_style_set_pad_hor(&s_style_button_sec_pressed, 10);
    lv_style_set_pad_ver(&s_style_button_sec_pressed, 6);

    lv_style_set_bg_opa(&s_style_switch_track, LV_OPA_COVER);
    lv_style_set_radius(&s_style_switch_track, LV_RADIUS_CIRCLE);

    lv_style_set_bg_opa(&s_style_switch_track_checked, LV_OPA_COVER);
    lv_style_set_radius(&s_style_switch_track_checked, LV_RADIUS_CIRCLE);

    lv_style_set_bg_opa(&s_style_switch_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_style_switch_knob, LV_RADIUS_CIRCLE);

    lv_style_set_bg_opa(&s_style_tile, LV_OPA_COVER);
    lv_style_set_radius(&s_style_tile, 12);
    lv_style_set_border_width(&s_style_tile, 0);
    lv_style_set_pad_all(&s_style_tile, 4);

    lv_style_set_bg_opa(&s_style_tile_pressed, LV_OPA_COVER);
    lv_style_set_radius(&s_style_tile_pressed, 12);
    lv_style_set_border_width(&s_style_tile_pressed, 0);
    lv_style_set_pad_all(&s_style_tile_pressed, 4);
}

static void styles_apply_palette(const ui_theme_palette_t *p)
{
    lv_style_set_bg_color(&s_style_screen, p->bg);
    lv_style_set_text_color(&s_style_screen, p->text);

    lv_style_set_bg_color(&s_style_surface, p->surface);
    lv_style_set_text_color(&s_style_surface, p->text);

    lv_style_set_bg_color(&s_style_surface_border, p->surface);
    lv_style_set_border_color(&s_style_surface_border, p->border);
    lv_style_set_text_color(&s_style_surface_border, p->text);

    lv_style_set_text_color(&s_style_text, p->text);
    lv_style_set_text_color(&s_style_text_muted, p->text_muted);

    lv_style_set_bg_color(&s_style_button, p->accent);
    lv_style_set_text_color(&s_style_button, p->accent_text);
    lv_color_t accent_pressed = lv_color_mix(p->accent, p->text, 40);
    lv_style_set_bg_color(&s_style_button_pressed, accent_pressed);
    lv_style_set_text_color(&s_style_button_pressed, p->accent_text);

    lv_style_set_bg_color(&s_style_button_sec, p->surface);
    lv_style_set_text_color(&s_style_button_sec, p->text);
    lv_style_set_border_color(&s_style_button_sec, p->border);
    lv_style_set_bg_color(&s_style_button_sec_pressed, lv_color_mix(p->accent, p->surface, 48));
    lv_style_set_text_color(&s_style_button_sec_pressed, p->text);
    lv_style_set_border_color(&s_style_button_sec_pressed, p->accent);

    lv_style_set_bg_color(&s_style_switch_track, p->border);
    lv_style_set_bg_color(&s_style_switch_track_checked, p->accent);
    lv_style_set_bg_color(&s_style_switch_knob, p->surface);

    lv_style_set_bg_color(&s_style_tile, p->surface);
    lv_style_set_border_color(&s_style_tile, p->border);
    lv_style_set_text_color(&s_style_tile, p->text);

    lv_color_t pressed = lv_color_mix(p->accent, p->surface, 64);
    lv_style_set_bg_color(&s_style_tile_pressed, pressed);
    lv_style_set_border_color(&s_style_tile_pressed, p->accent);
    lv_style_set_text_color(&s_style_tile_pressed, p->text);
}

static void apply_to_active(void)
{
    if (!s_disp) return;

    lv_obj_t *scr = lv_disp_get_scr_act(s_disp);
    if (scr) ui_theme_apply_screen(scr);
}

void ui_theme_init(lv_disp_t *disp)
{
    s_disp = disp;

    styles_init_once();
    s_palette = palette_light();
    styles_apply_palette(&s_palette);

    apply_to_active();
}

void ui_theme_set(ui_theme_t theme)
{
    s_theme = theme;
    ui_theme_palette_t p = (theme == UI_THEME_DARK) ? palette_dark() : palette_light();
    ui_theme_set_palette(&p);
}

ui_theme_t ui_theme_get(void)
{
    return s_theme;
}

void ui_theme_set_palette(const ui_theme_palette_t *palette)
{
    if (!palette) return;

    s_palette = *palette;
    styles_apply_palette(&s_palette);

    apply_to_active();
}

const ui_theme_palette_t *ui_theme_palette(void)
{
    return &s_palette;
}

void ui_theme_apply_screen(lv_obj_t *screen)
{
    if (!screen) return;
    lv_obj_remove_style(screen, &s_style_screen, 0);
    lv_obj_add_style(screen, &s_style_screen, 0);
}

void ui_theme_apply_surface(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_remove_style(obj, &s_style_surface, 0);
    lv_obj_add_style(obj, &s_style_surface, 0);
}

void ui_theme_apply_surface_border(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_remove_style(obj, &s_style_surface_border, 0);
    lv_obj_add_style(obj, &s_style_surface_border, 0);
}

void ui_theme_apply_label(lv_obj_t *label, bool muted)
{
    if (!label) return;
    lv_obj_remove_style(label, &s_style_text, 0);
    lv_obj_remove_style(label, &s_style_text_muted, 0);
    lv_obj_add_style(label, &s_style_text, 0);
    if (muted) lv_obj_add_style(label, &s_style_text_muted, 0);
}

void ui_theme_apply_button(lv_obj_t *btn)
{
    if (!btn) return;
    lv_obj_remove_style(btn, &s_style_button, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(btn, &s_style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_remove_style(btn, &s_style_button_sec, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(btn, &s_style_button_sec_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_style_button, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &s_style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_height(btn, 36);
}

void ui_theme_apply_button_secondary(lv_obj_t *btn)
{
    if (!btn) return;
    lv_obj_remove_style(btn, &s_style_button, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(btn, &s_style_button_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_remove_style(btn, &s_style_button_sec, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(btn, &s_style_button_sec_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(btn, &s_style_button_sec, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &s_style_button_sec_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_height(btn, 36);
}

void ui_theme_apply_stepper(lv_obj_t *btn)
{
    ui_theme_apply_button(btn);
    lv_obj_set_size(btn, 28, 28);
    lv_obj_set_style_min_width(btn, 28, 0);
    lv_obj_set_style_min_height(btn, 28, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
}

void ui_theme_apply_field(lv_obj_t *obj)
{
    if (!obj) return;
    const ui_theme_palette_t *p = &s_palette;
    lv_obj_set_style_bg_color(obj, p->surface, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, p->text, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, p->border, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_min_height(obj, 32, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, p->accent, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(obj, p->accent, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(obj, p->accent_text, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(obj, p->accent, LV_PART_SELECTED);

    lv_obj_t *list = NULL;
    if (lv_obj_check_type(obj, &lv_dropdown_class))
        list = lv_dropdown_get_list(obj);
    if (list) {
        lv_obj_set_style_bg_color(list, p->surface, 0);
        lv_obj_set_style_text_color(list, p->text, 0);
        lv_obj_set_style_border_color(list, p->border, 0);
        lv_obj_set_style_bg_color(list, p->accent, LV_PART_SELECTED);
        lv_obj_set_style_text_color(list, p->accent_text, LV_PART_SELECTED);
    }
}

void ui_theme_apply_switch(lv_obj_t *sw)
{
    if (!sw) return;
    lv_obj_remove_style(sw, &s_style_switch_track, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_remove_style(sw, &s_style_switch_track_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_remove_style(sw, &s_style_switch_knob, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_style(sw, &s_style_switch_track, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_style(sw, &s_style_switch_track_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(sw, &s_style_switch_knob, LV_PART_KNOB | LV_STATE_DEFAULT);
}

void ui_theme_apply_list_group(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    ui_theme_apply_surface(obj);
    lv_obj_set_width(obj, lv_pct(100));
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_row(obj, 0, 0);
    /* Do not clip to the rounded corner. LVGL does that with an offscreen
     * buffer the size of the whole card. A Settings group is taller than the
     * 80 KB UI heap, the buffer never allocates, and the refresh loop never
     * returns, so the task watchdog fires. */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_theme_apply_tile(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_remove_style(obj, &s_style_tile, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, &s_style_tile_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(obj, &s_style_tile, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, &s_style_tile_pressed, LV_PART_MAIN | LV_STATE_PRESSED);
}

lv_color_t ui_theme_color_work(void)
{
    return s_palette.work;
}

lv_color_t ui_theme_color_rest(void)
{
    return s_palette.rest;
}

lv_color_t ui_theme_color_alert(void)
{
    return s_palette.alert;
}

lv_color_t ui_theme_color_rec(void)
{
    return s_palette.rec;
}

lv_color_t ui_theme_color_accent(void)
{
    return s_palette.accent;
}
