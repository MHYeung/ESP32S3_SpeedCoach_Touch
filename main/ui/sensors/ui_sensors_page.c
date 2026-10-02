#include "ui_sensors_page.h"

#include "sensor_hub.h"
#include "ui_settings_page.h"
#include "ui_status_bar.h"
#include "ui_theme.h"

#include "esp_err.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_sensors";

#define ACT_PAIR   1
#define ACT_SECOND 2
#define ACT_FORGET 3

typedef struct {
    lv_obj_t *card;
    lv_obj_t *status;
    lv_obj_t *btn_pair;
    lv_obj_t *lbl_pair;
    lv_obj_t *btn_second;
    lv_obj_t *lbl_second;
    lv_obj_t *btn_forget;
    lv_obj_t *lbl_forget;
    lv_obj_t *btn_zero;
    lv_obj_t *btn_side;
    lv_obj_t *lbl_side;
} sensor_card_t;

static lv_obj_t *s_root = NULL;
static ui_status_bar_t s_sb;
static sensor_card_t s_cards[SENSOR_KIND_COUNT];
static lv_obj_t *s_list = NULL;
static lv_timer_t *s_timer = NULL;
static ui_page_t s_return_page = UI_PAGE_MENU;
static uint32_t s_seen_epoch = 0xFFFFFFFFu;
static sensor_kind_t s_list_kind = SENSOR_KIND_HR;
static bool s_bow_side;

static const char *kind_title(sensor_kind_t kind)
{
    return (kind == SENSOR_KIND_HR) ? "Heart rate" : "RowPod";
}

static void *btn_tag(sensor_kind_t kind, int act)
{
    return (void *)(uintptr_t)((((unsigned)kind) << 8) | (unsigned)act);
}

static void tag_decode(const void *tag, sensor_kind_t *kind, int *act)
{
    unsigned v = (unsigned)(uintptr_t)tag;
    *kind = (sensor_kind_t)((v >> 8) & 0xff);
    *act = (int)(v & 0xff);
}

static void zero_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = sensor_hub_rowpod_zero();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "zero: %s", esp_err_to_name(err));
    }
}

static void side_cb(lv_event_t *e)
{
    (void)e;
    s_bow_side = !s_bow_side;
    esp_err_t err = sensor_hub_rowpod_set_side(s_bow_side);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "side: %s", esp_err_to_name(err));
    }
    sensor_card_t *card = &s_cards[SENSOR_KIND_ROWPOD];
    if (card->lbl_side) {
        lv_label_set_text(card->lbl_side, s_bow_side ? "Bow" : "Stroke");
    }
}

static void result_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    esp_err_t err = sensor_hub_connect_result(idx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect[%u]: %s", (unsigned)idx, esp_err_to_name(err));
    }
}

static void card_click_cb(lv_event_t *e)
{
    sensor_kind_t kind;
    int act;
    tag_decode(lv_event_get_user_data(e), &kind, &act);
    if (kind >= SENSOR_KIND_COUNT) {
        return;
    }
    sensor_hub_slot_info_t info;
    sensor_hub_get_slot(kind, &info);
    esp_err_t err = ESP_OK;

    if (act == ACT_PAIR) {
        if (info.state == SENSOR_HUB_CONNECTED) {
            err = sensor_hub_disconnect(kind);
        } else if (info.saved && info.state != SENSOR_HUB_SCANNING) {
            err = sensor_hub_connect_saved(kind);
        } else {
            s_list_kind = kind;
            err = sensor_hub_start_scan(kind);
        }
    } else if (act == ACT_SECOND) {
        s_list_kind = kind;
        err = sensor_hub_start_scan(kind);
    } else if (act == ACT_FORGET) {
        err = sensor_hub_forget(kind);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sensor act %d: %s", act, esp_err_to_name(err));
    }
}

static void format_status(sensor_kind_t kind, const sensor_hub_slot_info_t *info,
                          const sensor_hub_live_t *live, char *buf, size_t len)
{
    const char *name = info->name[0] ? info->name : kind_title(kind);
    if (info->state == SENSOR_HUB_DISABLED) {
        snprintf(buf, len, "Bluetooth off in this build");
        return;
    }
    if (info->state == SENSOR_HUB_SCANNING) {
        snprintf(buf, len, "Scanning...");
        return;
    }
    if (info->state == SENSOR_HUB_CONNECTING) {
        snprintf(buf, len, "Connecting to %s...", name);
        return;
    }
    if (info->state == SENSOR_HUB_CONNECTED) {
        if (kind == SENSOR_KIND_HR && live->hr_bpm > 0) {
            if (info->battery_pct <= 100) {
                snprintf(buf, len, "%s  %u bpm  %u%%", name,
                         (unsigned)live->hr_bpm, (unsigned)info->battery_pct);
            } else {
                snprintf(buf, len, "%s  %u bpm", name, (unsigned)live->hr_bpm);
            }
        } else if (info->battery_pct <= 100) {
            snprintf(buf, len, "%s  connected  %u%%", name, (unsigned)info->battery_pct);
        } else {
            snprintf(buf, len, "%s  connected", name);
        }
        return;
    }
    if (info->saved) {
        snprintf(buf, len, info->paused ? "%s  paused" : "%s  saved", name);
        return;
    }
    snprintf(buf, len, "Not paired");
}

static void sync_card(sensor_kind_t kind, const sensor_hub_live_t *live)
{
    sensor_card_t *card = &s_cards[kind];
    if (!card->status) {
        return;
    }
    sensor_hub_slot_info_t info;
    sensor_hub_get_slot(kind, &info);
    char buf[64];
    format_status(kind, &info, live, buf, sizeof(buf));
    lv_label_set_text(card->status, buf);

    bool connected = (info.state == SENSOR_HUB_CONNECTED);
    bool scanning = (info.state == SENSOR_HUB_SCANNING);
    bool busy = scanning || (info.state == SENSOR_HUB_CONNECTING);

    if (connected) {
        lv_label_set_text(card->lbl_pair, "Disconnect");
    } else if (info.saved) {
        lv_label_set_text(card->lbl_pair, busy ? "Wait" : "Reconnect");
    } else {
        lv_label_set_text(card->lbl_pair, scanning ? "Scanning" : "Pair");
    }
    if (busy && !connected) {
        lv_obj_add_state(card->btn_pair, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(card->btn_pair, LV_STATE_DISABLED);
    }

    if (connected || !info.saved) {
        lv_obj_add_flag(card->btn_second, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(card->btn_second, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(card->lbl_second, "Pair");
        if (busy) {
            lv_obj_add_state(card->btn_second, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(card->btn_second, LV_STATE_DISABLED);
        }
    }

    if (info.saved || connected) {
        lv_obj_remove_flag(card->btn_forget, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(card->btn_forget, LV_OBJ_FLAG_HIDDEN);
    }
    if (card->btn_zero) {
        if (connected) {
            lv_obj_remove_flag(card->btn_zero, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(card->btn_side, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(card->btn_zero, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(card->btn_side, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void rebuild_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);

    sensor_kind_t scanning = SENSOR_KIND_COUNT;
    bool active = sensor_hub_is_scanning(&scanning);

    sensor_hub_device_t devs[6];
    size_t n = sensor_hub_get_results(devs, 6);
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, active ? (scanning == SENSOR_KIND_HR ? "No heart-rate straps yet"
                                                                      : "No RowPods yet")
                                        : "Pair scans for this sensor only");
        ui_theme_apply_label(empty, true);
        return;
    }
    if (!active) {
        lv_obj_t *hint = lv_label_create(s_list);
        lv_label_set_text(hint, "Tap a device to connect");
        ui_theme_apply_label(hint, true);
    }
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        ui_theme_apply_list_group(row);
        lv_obj_set_height(row, 40);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_add_event_cb(row, result_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lb = lv_label_create(row);
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  %d dBm",
                 devs[i].name[0] ? devs[i].name : kind_title(devs[i].kind),
                 (int)devs[i].rssi);
        lv_label_set_text(lb, buf);
        lv_obj_center(lb);
    }
}

static void refresh(void)
{
    sensor_hub_live_t live;
    sensor_hub_get_live(&live);
    for (int i = 0; i < SENSOR_KIND_COUNT; i++) {
        sync_card((sensor_kind_t)i, &live);
    }
    rebuild_list();
    settings_page_sync_sensors_state();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_root) {
        return;
    }
    uint32_t epoch = sensor_hub_get_epoch();
    bool changed = (epoch != s_seen_epoch);
    if (ui_get_current_page() != UI_SENSORS_PAGE) {
        if (changed) {
            s_seen_epoch = epoch;
            settings_page_sync_sensors_state();
        }
        return;
    }
    if (changed) {
        s_seen_epoch = epoch;
        refresh();
        return;
    }
    /* BPM goes stale without an epoch bump. Refresh the status lines only. */
    sensor_hub_live_t live;
    sensor_hub_get_live(&live);
    for (int i = 0; i < SENSOR_KIND_COUNT; i++) {
        sensor_card_t *card = &s_cards[i];
        if (!card->status) {
            continue;
        }
        sensor_hub_slot_info_t info;
        sensor_hub_get_slot((sensor_kind_t)i, &info);
        char buf[64];
        format_status((sensor_kind_t)i, &info, &live, buf, sizeof(buf));
        const char *cur = lv_label_get_text(card->status);
        if (!cur || strcmp(cur, buf) != 0) {
            lv_label_set_text(card->status, buf);
        }
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, sensor_kind_t kind, int act, const char *text,
                          lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    ui_theme_apply_button(btn);
    lv_obj_set_height(btn, 36);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_pad_hor(btn, 2, 0);
    lv_obj_add_event_cb(btn, card_click_cb, LV_EVENT_CLICKED, btn_tag(kind, act));
    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, lv_pct(100));
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lb);
    *label_out = lb;
    return btn;
}

static void make_card(lv_obj_t *parent, sensor_kind_t kind)
{
    sensor_card_t *card = &s_cards[kind];
    memset(card, 0, sizeof(*card));

    card->card = lv_obj_create(parent);
    ui_theme_apply_surface(card->card);
    lv_obj_set_width(card->card, lv_pct(100));
    lv_obj_set_height(card->card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card->card, 6, 0);
    lv_obj_set_style_pad_row(card->card, 4, 0);
    lv_obj_set_flex_flow(card->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card->card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card->card);
    lv_label_set_text(title, kind_title(kind));
    ui_theme_apply_label(title, false);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    card->status = lv_label_create(card->card);
    lv_label_set_text(card->status, "Not paired");
    ui_theme_apply_label(card->status, true);
    lv_label_set_long_mode(card->status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(card->status, lv_pct(100));

    lv_obj_t *row = lv_obj_create(card->card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 40);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    card->btn_pair = make_btn(row, kind, ACT_PAIR, "Pair", &card->lbl_pair);
    card->btn_second = make_btn(row, kind, ACT_SECOND, "Pair", &card->lbl_second);
    card->btn_forget = make_btn(row, kind, ACT_FORGET, "Forget", &card->lbl_forget);
    lv_obj_add_flag(card->btn_second, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card->btn_forget, LV_OBJ_FLAG_HIDDEN);

    if (kind == SENSOR_KIND_ROWPOD) {
        lv_obj_t *oar = lv_obj_create(card->card);
        lv_obj_set_width(oar, lv_pct(100));
        lv_obj_set_height(oar, 40);
        lv_obj_set_style_bg_opa(oar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(oar, 0, 0);
        lv_obj_set_style_pad_all(oar, 0, 0);
        lv_obj_set_style_pad_column(oar, 4, 0);
        lv_obj_set_flex_flow(oar, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(oar, LV_OBJ_FLAG_SCROLLABLE);
        card->btn_zero = lv_btn_create(oar);
        ui_theme_apply_button(card->btn_zero);
        lv_obj_set_height(card->btn_zero, 36);
        lv_obj_set_flex_grow(card->btn_zero, 1);
        lv_obj_add_event_cb(card->btn_zero, zero_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *zl = lv_label_create(card->btn_zero);
        lv_label_set_text(zl, "Zero oar");
        lv_obj_set_style_text_font(zl, &lv_font_montserrat_16, 0);
        lv_obj_center(zl);
        card->btn_side = lv_btn_create(oar);
        ui_theme_apply_button(card->btn_side);
        lv_obj_set_height(card->btn_side, 36);
        lv_obj_set_flex_grow(card->btn_side, 1);
        lv_obj_add_event_cb(card->btn_side, side_cb, LV_EVENT_CLICKED, NULL);
        card->lbl_side = lv_label_create(card->btn_side);
        lv_label_set_text(card->lbl_side, "Stroke");
        lv_obj_set_style_text_font(card->lbl_side, &lv_font_montserrat_16, 0);
        lv_obj_center(card->lbl_side);
        lv_obj_add_flag(card->btn_zero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card->btn_side, LV_OBJ_FLAG_HIDDEN);
    }
}

void sensors_page_set_return_page(ui_page_t page)
{
    s_return_page = page;
    if (s_root) {
        ui_status_bar_set_title(&s_sb, "Sensors", page);
    }
}

ui_page_t sensors_page_get_return_page(void)
{
    return s_return_page;
}

void sensors_page_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_create(&s_sb, s_root);
    ui_status_bar_set_title(&s_sb, "Sensors", s_return_page);

    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 4, 0);
    lv_obj_set_style_pad_row(body, 4, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(body, 36, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    make_card(body, SENSOR_KIND_HR);
    make_card(body, SENSOR_KIND_ROWPOD);

    s_list = lv_obj_create(body);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);

    s_seen_epoch = 0xFFFFFFFFu;
    s_timer = lv_timer_create(tick_cb, 300, NULL);
    refresh();
}

void sensors_page_apply_theme(void)
{
    if (!s_root) {
        return;
    }
    ui_status_bar_apply_theme(&s_sb);
    for (int i = 0; i < SENSOR_KIND_COUNT; i++) {
        sensor_card_t *card = &s_cards[i];
        if (card->card) {
            ui_theme_apply_surface(card->card);
        }
        if (card->status) {
            ui_theme_apply_label(card->status, true);
        }
        if (card->btn_pair) {
            ui_theme_apply_button(card->btn_pair);
        }
        if (card->btn_second) {
            ui_theme_apply_button(card->btn_second);
        }
        if (card->btn_forget) {
            ui_theme_apply_button(card->btn_forget);
        }
        if (card->btn_zero) {
            ui_theme_apply_button(card->btn_zero);
        }
        if (card->btn_side) {
            ui_theme_apply_button(card->btn_side);
        }
    }
}

void sensors_page_on_orientation_changed(void)
{
    if (!s_root) {
        return;
    }
    ui_status_bar_force_refresh(&s_sb);
}
