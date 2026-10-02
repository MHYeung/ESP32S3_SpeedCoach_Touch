#include "sensor_hub.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_helper.h"

#include <stdio.h>
#include <string.h>

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && defined(CONFIG_BT_NIMBLE_ENABLED)

#include "sensor_hub_priv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "sensor_hub";

/* RowPod motion service 9b7e1000-2b2f-4f71-9b86-4bb2e6d54f00 (LE wire order). */
static const ble_uuid128_t s_uuid_motion =
    BLE_UUID128_INIT(0x00, 0x4f, 0xd5, 0xe6, 0xb2, 0x4b, 0x86, 0x9b,
                     0x71, 0x4f, 0x2f, 0x2b, 0x00, 0x10, 0x7e, 0x9b);

#define UUID_HRS           0x180D
#define SCAN_MS            6000
#define CONNECT_MS         10000
#define HR_STALE_US        5000000LL
#define HR_BACKOFF_MIN_MS  2000
#define HR_BACKOFF_MAX_MS  15000
#define HR_BACKOFF_BOOT_MS 500

typedef enum {
    GAP_IDLE = 0,
    GAP_SCANNING,
    GAP_CONNECTING,
} gap_phase_t;

typedef enum {
    PEND_NONE = 0,
    PEND_SCAN,
    PEND_CONNECT,
} pend_t;

static SemaphoreHandle_t s_mux;
static uint32_t s_epoch;
static uint8_t s_own_addr_type;
static bool s_synced;
static bool s_available;
static gap_phase_t s_gap;
static sensor_kind_t s_scan_kind;
static sensor_kind_t s_connecting_kind;
static pend_t s_pend;
static sensor_kind_t s_pend_kind;
static sensor_hub_device_t s_pend_dev;

static sensor_slot_t s_slots[SENSOR_KIND_COUNT];
static sensor_hub_device_t s_found[SENSOR_HUB_MAX_RESULTS];
static size_t s_found_n;

static struct ble_npl_callout s_hr_reconn;
static bool s_callout_ready;

static int gap_event(struct ble_gap_event *event, void *arg);
static void arm_hr_if_needed(void);
static esp_err_t begin_scan(sensor_kind_t kind);
static esp_err_t issue_connect(sensor_kind_t kind, const sensor_hub_device_t *dev);

static void bump_locked(void)
{
    s_epoch++;
}

static bool addr_nonzero(const uint8_t addr[6])
{
    for (int i = 0; i < 6; i++) {
        if (addr[i] != 0) {
            return true;
        }
    }
    return false;
}

static bool addr_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static void copy_name(char *dst, size_t dst_len, const uint8_t *name, uint8_t name_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!name || name_len == 0) {
        return;
    }
    size_t n = name_len;
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, name, n);
    dst[n] = '\0';
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
    (void)param;
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble reset reason=%d", reason);
    s_synced = false;
}

static bool slot_wants_link_locked(sensor_kind_t kind)
{
    const sensor_slot_t *slot = &s_slots[kind];
    return slot->saved && !slot->paused &&
           slot->state != SENSOR_HUB_CONNECTED &&
           slot->state != SENSOR_HUB_CONNECTING;
}

static sensor_kind_t reconnect_kind_locked(void)
{
    if (slot_wants_link_locked(SENSOR_KIND_HR)) {
        return SENSOR_KIND_HR;
    }
    if (slot_wants_link_locked(SENSOR_KIND_ROWPOD)) {
        return SENSOR_KIND_ROWPOD;
    }
    return SENSOR_KIND_COUNT;
}

static void arm_hr_if_needed(void)
{
    if (!s_callout_ready || !s_synced || !s_mux) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    sensor_kind_t kind = reconnect_kind_locked();
    bool want = (kind < SENSOR_KIND_COUNT);
    bool busy = (s_gap != GAP_IDLE) || (s_pend != PEND_NONE);
    uint32_t delay = want ? s_slots[kind].backoff_ms : 0;
    xSemaphoreGive(s_mux);
    if (!want) {
        ble_npl_callout_stop(&s_hr_reconn);
        return;
    }
    if (delay == 0) {
        delay = HR_BACKOFF_MIN_MS;
    }
    if (busy) {
        delay = 1000;
    }
    ble_npl_callout_reset(&s_hr_reconn, ble_npl_time_ms_to_ticks32(delay));
}

static void note_failure_locked(sensor_kind_t kind)
{
    sensor_slot_t *slot = &s_slots[kind];
    if (!slot_wants_link_locked(kind)) {
        return;
    }
    if (slot->backoff_ms < HR_BACKOFF_MIN_MS) {
        slot->backoff_ms = HR_BACKOFF_MIN_MS;
    } else if (slot->backoff_ms < HR_BACKOFF_MAX_MS) {
        uint32_t next = slot->backoff_ms * 2;
        slot->backoff_ms = next > HR_BACKOFF_MAX_MS ? HR_BACKOFF_MAX_MS : next;
    }
}

static void persist_slot(const sensor_slot_t *slot)
{
    if (slot->kind == SENSOR_KIND_HR) {
        nvs_helper_set_hr_addr(slot->addr);
        nvs_helper_set_hr_addr_type(slot->addr_type);
        nvs_helper_set_hr_name(slot->name);
    } else {
        nvs_helper_set_sensor_addr(slot->addr);
        nvs_helper_set_sensor_addr_type(slot->addr_type);
        nvs_helper_set_sensor_name(slot->name);
    }
    bool any = s_slots[SENSOR_KIND_HR].saved || s_slots[SENSOR_KIND_ROWPOD].saved;
    nvs_helper_set_sensors_enabled(any);
}

static void clear_persisted(sensor_kind_t kind)
{
    uint8_t zero[6] = {0};
    if (kind == SENSOR_KIND_HR) {
        nvs_helper_set_hr_addr(zero);
        nvs_helper_set_hr_addr_type(0);
        nvs_helper_set_hr_name("");
    } else {
        nvs_helper_set_sensor_addr(zero);
        nvs_helper_set_sensor_addr_type(0);
        nvs_helper_set_sensor_name("");
    }
    bool any = s_slots[SENSOR_KIND_HR].saved || s_slots[SENSOR_KIND_ROWPOD].saved;
    nvs_helper_set_sensors_enabled(any);
}

static void load_slot(sensor_kind_t kind)
{
    sensor_slot_t *slot = &s_slots[kind];
    slot->kind = kind;
    slot->state = SENSOR_HUB_IDLE;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->battery_pct = 255;
    slot->backoff_ms = HR_BACKOFF_BOOT_MS;
    if (kind == SENSOR_KIND_HR) {
        nvs_helper_get_hr_addr(slot->addr);
        slot->addr_type = nvs_helper_get_hr_addr_type();
        nvs_helper_get_hr_name(slot->name, sizeof(slot->name));
    } else {
        nvs_helper_get_sensor_addr(slot->addr);
        slot->addr_type = nvs_helper_get_sensor_addr_type();
        nvs_helper_get_sensor_name(slot->name, sizeof(slot->name));
    }
    slot->saved = addr_nonzero(slot->addr);
    if (slot->saved && slot->name[0] == '\0') {
        snprintf(slot->name, sizeof(slot->name),
                 kind == SENSOR_KIND_HR ? "Heart rate" : "RowPod");
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE address rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "addr type rc=%d", rc);
        return;
    }
    s_synced = true;
    ESP_LOGI(TAG, "nimble synced addr_type=%u heap=%u",
             (unsigned)s_own_addr_type,
             (unsigned)esp_get_free_heap_size());
    arm_hr_if_needed();
}

static sensor_kind_t classify(const struct ble_hs_adv_fields *fields)
{
    bool hr = false;
    bool pod = false;
    for (uint8_t i = 0; i < fields->num_uuids16; i++) {
        if (fields->uuids16[i].value == UUID_HRS) {
            hr = true;
        }
    }
    if (fields->svc_data_uuid16 && fields->svc_data_uuid16_len >= 2) {
        uint16_t u = (uint16_t)fields->svc_data_uuid16[0] |
                     ((uint16_t)fields->svc_data_uuid16[1] << 8);
        if (u == UUID_HRS) {
            hr = true;
        }
    }
    for (uint8_t i = 0; i < fields->num_uuids128; i++) {
        if (ble_uuid_cmp(&fields->uuids128[i].u, &s_uuid_motion.u) == 0) {
            pod = true;
        }
    }
    if (hr) {
        return SENSOR_KIND_HR;
    }
    if (pod) {
        return SENSOR_KIND_ROWPOD;
    }
    return SENSOR_KIND_COUNT;
}

static void upsert_result(const struct ble_gap_disc_desc *disc,
                          const struct ble_hs_adv_fields *fields,
                          bool allow_insert)
{
    sensor_kind_t kind = classify(fields);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int slot = -1;
    for (size_t i = 0; i < s_found_n; i++) {
        if (addr_eq(s_found[i].addr, disc->addr.val)) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        if (!allow_insert || kind != s_scan_kind || s_found_n >= SENSOR_HUB_MAX_RESULTS) {
            xSemaphoreGive(s_mux);
            return;
        }
        slot = (int)s_found_n++;
        memset(&s_found[slot], 0, sizeof(s_found[slot]));
        memcpy(s_found[slot].addr, disc->addr.val, 6);
        s_found[slot].addr_type = disc->addr.type;
        s_found[slot].kind = kind;
        snprintf(s_found[slot].name, sizeof(s_found[slot].name),
                 kind == SENSOR_KIND_HR ? "Heart rate" : "RowPod");
        bump_locked();
    } else if (kind == s_scan_kind) {
        s_found[slot].kind = kind;
    }
    char old_name[SENSOR_NAME_LEN];
    memcpy(old_name, s_found[slot].name, sizeof(old_name));
    s_found[slot].rssi = disc->rssi;
    s_found[slot].addr_type = disc->addr.type;
    copy_name(s_found[slot].name, sizeof(s_found[slot].name), fields->name, fields->name_len);
    if (memcmp(old_name, s_found[slot].name, sizeof(old_name)) != 0) {
        bump_locked();
    }
    xSemaphoreGive(s_mux);
}

static void run_pending(void)
{
    pend_t pend;
    sensor_kind_t kind;
    sensor_hub_device_t dev;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    pend = s_pend;
    kind = s_pend_kind;
    dev = s_pend_dev;
    s_pend = PEND_NONE;
    s_gap = GAP_IDLE;
    xSemaphoreGive(s_mux);

    if (pend == PEND_SCAN) {
        (void)begin_scan(kind);
        return;
    }
    if (pend == PEND_CONNECT) {
        (void)issue_connect(kind, &dev);
        return;
    }
    arm_hr_if_needed();
}

static void hr_reconn_cb(struct ble_npl_event *ev)
{
    (void)ev;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    sensor_kind_t kind = reconnect_kind_locked();
    bool ready = kind < SENSOR_KIND_COUNT && s_gap == GAP_IDLE && s_pend == PEND_NONE && s_synced;
    sensor_hub_device_t dev = {0};
    if (ready) {
        sensor_slot_t *slot = &s_slots[kind];
        memcpy(dev.addr, slot->addr, 6);
        dev.addr_type = slot->addr_type;
        dev.kind = kind;
        snprintf(dev.name, sizeof(dev.name), "%s",
                 slot->name[0] ? slot->name : (kind == SENSOR_KIND_HR ? "Heart rate" : "RowPod"));
    }
    xSemaphoreGive(s_mux);
    if (!ready) {
        arm_hr_if_needed();
        return;
    }
    if (issue_connect(kind, &dev) != ESP_OK) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        note_failure_locked(kind);
        xSemaphoreGive(s_mux);
        arm_hr_if_needed();
    }
}

static esp_err_t begin_scan(sensor_kind_t kind)
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_found_n = 0;
    s_scan_kind = kind;
    s_gap = GAP_SCANNING;
    bump_locked();
    xSemaphoreGive(s_mux);

    struct ble_gap_disc_params dp = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, SCAN_MS, &dp, gap_event, NULL);
    if (rc != 0) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_gap = GAP_IDLE;
        bump_locked();
        xSemaphoreGive(s_mux);
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        arm_hr_if_needed();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "scanning kind=%d", (int)kind);
    return ESP_OK;
}

static esp_err_t issue_connect(sensor_kind_t kind, const sensor_hub_device_t *dev)
{
    if (!s_synced || !dev) {
        return ESP_ERR_INVALID_STATE;
    }
    sensor_slot_t *slot = &s_slots[kind];
    if (slot->state == SENSOR_HUB_CONNECTED || slot->state == SENSOR_HUB_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_addr_t addr = {.type = dev->addr_type};
    memcpy(addr.val, dev->addr, 6);

    xSemaphoreTake(s_mux, portMAX_DELAY);
    memcpy(slot->addr, dev->addr, 6);
    slot->addr_type = dev->addr_type;
    if (dev->name[0]) {
        snprintf(slot->name, sizeof(slot->name), "%s", dev->name);
    }
    slot->rssi = dev->rssi;
    slot->state = SENSOR_HUB_CONNECTING;
    slot->paused = false;
    s_connecting_kind = kind;
    s_gap = GAP_CONNECTING;
    bump_locked();
    xSemaphoreGive(s_mux);

    int rc = ble_gap_connect(s_own_addr_type, &addr, CONNECT_MS, NULL, gap_event, slot);
    if (rc != 0) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        slot->state = SENSOR_HUB_IDLE;
        s_gap = GAP_IDLE;
        bump_locked();
        xSemaphoreGive(s_mux);
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_connect(sensor_kind_t kind, const sensor_hub_device_t *dev)
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_slots[kind].state == SENSOR_HUB_CONNECTED ||
        s_slots[kind].state == SENSOR_HUB_CONNECTING) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    bool scanning = (s_gap == GAP_SCANNING);
    bool connecting = (s_gap == GAP_CONNECTING);
    if (scanning || connecting) {
        s_pend = PEND_CONNECT;
        s_pend_kind = kind;
        s_pend_dev = *dev;
        xSemaphoreGive(s_mux);
        if (scanning) {
            (void)ble_gap_disc_cancel();
        } else {
            (void)ble_gap_conn_cancel();
        }
        return ESP_OK;
    }
    xSemaphoreGive(s_mux);
    return issue_connect(kind, dev);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields fields;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            bool identified = classify(&fields) != SENSOR_KIND_COUNT;
            upsert_result(&event->disc, &fields, identified);
        } else if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                   event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND) {
            upsert_result(&event->disc, &fields, true);
        }
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "scan done n=%u", (unsigned)s_found_n);
        xSemaphoreTake(s_mux, portMAX_DELAY);
        /* One advertiser is the pod or strap the user just asked for. */
        if (s_pend == PEND_NONE && s_found_n == 1) {
            s_pend = PEND_CONNECT;
            s_pend_kind = s_found[0].kind;
            s_pend_dev = s_found[0];
            ESP_LOGI(TAG, "auto-connect %s",
                     s_found[0].name[0] ? s_found[0].name : "sensor");
        }
        bump_locked();
        xSemaphoreGive(s_mux);
        run_pending();
        return 0;

    case BLE_GAP_EVENT_CONNECT: {
        sensor_slot_t *slot = arg;
        bool ok = (event->connect.status == 0);
        sensor_slot_t saved = {0};
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_gap = GAP_IDLE;
        if (slot) {
            if (ok) {
                slot->conn_handle = event->connect.conn_handle;
                slot->state = SENSOR_HUB_CONNECTED;
                slot->saved = true;
                slot->paused = false;
                saved = *slot;
                ESP_LOGI(TAG, "connected %s heap=%u",
                         slot->name[0] ? slot->name : "sensor",
                         (unsigned)esp_get_free_heap_size());
            } else {
                slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
                slot->state = SENSOR_HUB_IDLE;
                slot->hr_bpm = 0;
                slot->hr_us = 0;
                if (s_pend != PEND_SCAN && s_pend != PEND_CONNECT) {
                    note_failure_locked(slot->kind);
                }
                ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            }
        }
        bump_locked();
        xSemaphoreGive(s_mux);
        if (ok && slot) {
            persist_slot(&saved);
            if (slot->kind == SENSOR_KIND_HR) {
                sensor_hub_hrs_on_connect(slot);
            } else if (slot->kind == SENSOR_KIND_ROWPOD) {
                sensor_hub_rowpod_on_connect(slot);
            }
        }
        run_pending();
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        sensor_slot_t *slot = arg;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (slot) {
            slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
            slot->state = SENSOR_HUB_IDLE;
            slot->hr_bpm = 0;
            slot->hr_us = 0;
            slot->hrs_val_handle = 0;
            slot->pod_valid = false;
            if (s_gap == GAP_CONNECTING && s_connecting_kind == slot->kind) {
                s_gap = GAP_IDLE;
            }
        }
        bump_locked();
        xSemaphoreGive(s_mux);
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        if (s_pend != PEND_NONE) {
            run_pending();
        } else {
            arm_hr_if_needed();
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        sensor_slot_t *slot = arg;
        if (slot && event->notify_rx.conn_handle == slot->conn_handle) {
            if (slot->kind == SENSOR_KIND_HR) {
                sensor_hub_hrs_on_notify(slot, event->notify_rx.om);
            } else if (slot->kind == SENSOR_KIND_ROWPOD) {
                sensor_hub_rowpod_on_notify(slot, event->notify_rx.attr_handle,
                                            event->notify_rx.om);
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

void sensor_hub_slot_set_hr(sensor_slot_t *slot, uint16_t bpm)
{
    if (!slot) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = slot->hr_bpm != bpm;
    slot->hr_bpm = bpm;
    slot->hr_us = (bpm > 0) ? esp_timer_get_time() : 0;
    if (changed) {
        bump_locked();
    }
    xSemaphoreGive(s_mux);
}

void sensor_hub_slot_set_battery(sensor_slot_t *slot, uint8_t pct)
{
    if (!slot) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    slot->battery_pct = pct;
    bump_locked();
    xSemaphoreGive(s_mux);
}

void sensor_hub_slot_drop(sensor_slot_t *slot)
{
    if (!slot || slot->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    int rc = ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "terminate rc=%d", rc);
    }
}

uint16_t sensor_hub_slot_conn(sensor_kind_t kind)
{
    if (kind >= SENSOR_KIND_COUNT || !s_mux) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint16_t handle = s_slots[kind].conn_handle;
    if (s_slots[kind].state != SENSOR_HUB_CONNECTED) {
        handle = BLE_HS_CONN_HANDLE_NONE;
    }
    xSemaphoreGive(s_mux);
    return handle;
}

void sensor_hub_slot_set_pod(sensor_slot_t *slot, bool valid, int16_t catch_ddeg,
                             int16_t finish_ddeg, uint16_t arc_ddeg, uint16_t spm_x10)
{
    if (!slot) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    slot->pod_valid = valid;
    slot->pod_catch_ddeg = catch_ddeg;
    slot->pod_finish_ddeg = finish_ddeg;
    slot->pod_arc_ddeg = arc_ddeg;
    slot->pod_spm_x10 = spm_x10;
    bump_locked();
    xSemaphoreGive(s_mux);
}

void sensor_hub_hrs_subscribed(sensor_slot_t *slot)
{
    if (!slot) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    slot->backoff_ms = HR_BACKOFF_MIN_MS;
    xSemaphoreGive(s_mux);
}

esp_err_t sensor_hub_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < SENSOR_KIND_COUNT; i++) {
        load_slot((sensor_kind_t)i);
    }

    ESP_LOGI(TAG, "heap before NimBLE: %u", (unsigned)esp_get_free_heap_size());
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "heap after NimBLE: %u", (unsigned)esp_get_free_heap_size());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    if (ble_npl_callout_init(&s_hr_reconn, nimble_port_get_dflt_eventq(),
                             hr_reconn_cb, NULL) != 0) {
        ESP_LOGE(TAG, "reconnect callout init failed");
    } else {
        s_callout_ready = true;
    }
    nimble_port_freertos_init(host_task);
    s_available = true;
    ESP_LOGI(TAG, "sensor hub ready (HR + RowPod GAP)");
    return ESP_OK;
}

bool sensor_hub_available(void)
{
    return s_available;
}

uint32_t sensor_hub_get_epoch(void)
{
    return s_epoch;
}

esp_err_t sensor_hub_start_scan(sensor_kind_t kind)
{
    if (!s_available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (kind >= SENSOR_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_slots[kind].state == SENSOR_HUB_CONNECTED ||
        s_slots[kind].state == SENSOR_HUB_CONNECTING) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_gap == GAP_CONNECTING) {
        s_pend = PEND_SCAN;
        s_pend_kind = kind;
        xSemaphoreGive(s_mux);
        (void)ble_gap_conn_cancel();
        return ESP_OK;
    }
    if (s_gap == GAP_SCANNING) {
        s_pend = PEND_SCAN;
        s_pend_kind = kind;
        xSemaphoreGive(s_mux);
        (void)ble_gap_disc_cancel();
        return ESP_OK;
    }
    xSemaphoreGive(s_mux);
    return begin_scan(kind);
}

esp_err_t sensor_hub_stop_scan(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool scanning = (s_gap == GAP_SCANNING);
    if (scanning) {
        s_pend = PEND_NONE;
    }
    xSemaphoreGive(s_mux);
    if (scanning) {
        (void)ble_gap_disc_cancel();
    }
    return ESP_OK;
}

bool sensor_hub_is_scanning(sensor_kind_t *kind_out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool scanning = (s_gap == GAP_SCANNING);
    if (kind_out && scanning) {
        *kind_out = s_scan_kind;
    }
    xSemaphoreGive(s_mux);
    return scanning;
}

size_t sensor_hub_get_results(sensor_hub_device_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    size_t n = s_found_n < max ? s_found_n : max;
    memcpy(out, s_found, n * sizeof(*out));
    xSemaphoreGive(s_mux);
    return n;
}

esp_err_t sensor_hub_connect_result(size_t index)
{
    sensor_hub_device_t dev;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (index >= s_found_n) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_NOT_FOUND;
    }
    dev = s_found[index];
    xSemaphoreGive(s_mux);
    return start_connect(dev.kind, &dev);
}

esp_err_t sensor_hub_connect_saved(sensor_kind_t kind)
{
    if (kind >= SENSOR_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_hub_device_t dev = {0};
    xSemaphoreTake(s_mux, portMAX_DELAY);
    sensor_slot_t *slot = &s_slots[kind];
    if (!slot->saved || !addr_nonzero(slot->addr)) {
        xSemaphoreGive(s_mux);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dev.addr, slot->addr, 6);
    dev.addr_type = slot->addr_type;
    dev.kind = kind;
    snprintf(dev.name, sizeof(dev.name), "%s", slot->name[0] ? slot->name : "Saved");
    slot->paused = false;
    if (kind == SENSOR_KIND_HR) {
        slot->backoff_ms = HR_BACKOFF_BOOT_MS;
    }
    xSemaphoreGive(s_mux);
    return start_connect(kind, &dev);
}

static void release_link(sensor_kind_t kind, bool forget)
{
    uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
    bool cancel = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    sensor_slot_t *slot = &s_slots[kind];
    slot->paused = true;
    if (forget) {
        slot->saved = false;
        memset(slot->addr, 0, sizeof(slot->addr));
        slot->name[0] = '\0';
        slot->battery_pct = 255;
    }
    handle = slot->conn_handle;
    cancel = (s_gap == GAP_CONNECTING && s_connecting_kind == kind);
    if (slot->state != SENSOR_HUB_CONNECTED) {
        slot->state = SENSOR_HUB_IDLE;
    }
    bump_locked();
    xSemaphoreGive(s_mux);
    if (forget) {
        clear_persisted(kind);
    }
    if (cancel) {
        (void)ble_gap_conn_cancel();
    } else if (handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    arm_hr_if_needed();
}

esp_err_t sensor_hub_disconnect(sensor_kind_t kind)
{
    if (kind >= SENSOR_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    release_link(kind, false);
    return ESP_OK;
}

esp_err_t sensor_hub_forget(sensor_kind_t kind)
{
    if (kind >= SENSOR_KIND_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    release_link(kind, true);
    return ESP_OK;
}

void sensor_hub_get_slot(sensor_kind_t kind, sensor_hub_slot_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->battery_pct = 255;
    if (kind >= SENSOR_KIND_COUNT || !s_mux) {
        out->state = s_available ? SENSOR_HUB_IDLE : SENSOR_HUB_DISABLED;
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const sensor_slot_t *slot = &s_slots[kind];
    out->state = slot->state;
    if (s_gap == GAP_SCANNING && s_scan_kind == kind && slot->state == SENSOR_HUB_IDLE) {
        out->state = SENSOR_HUB_SCANNING;
    }
    out->saved = slot->saved;
    out->paused = slot->paused;
    out->battery_pct = slot->battery_pct;
    snprintf(out->name, sizeof(out->name), "%s", slot->name);
    xSemaphoreGive(s_mux);
}

void sensor_hub_get_live(sensor_hub_live_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mux) {
        return;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const sensor_slot_t *hr = &s_slots[SENSOR_KIND_HR];
    const sensor_slot_t *pod = &s_slots[SENSOR_KIND_ROWPOD];
    out->hr_saved = hr->saved;
    out->hr_paused = hr->paused;
    out->hr_connected = (hr->state == SENSOR_HUB_CONNECTED);
    out->pod_connected = (pod->state == SENSOR_HUB_CONNECTED);
    out->pod_valid = pod->pod_valid;
    out->pod_catch_ddeg = pod->pod_catch_ddeg;
    out->pod_finish_ddeg = pod->pod_finish_ddeg;
    out->pod_arc_ddeg = pod->pod_arc_ddeg;
    out->pod_spm_x10 = pod->pod_spm_x10;
    if (hr->hr_us != 0 && (esp_timer_get_time() - hr->hr_us) <= HR_STALE_US) {
        out->hr_bpm = hr->hr_bpm;
    }
    xSemaphoreGive(s_mux);
}

#else /* Bluetooth disabled */

static const char *TAG = "sensor_hub";

esp_err_t sensor_hub_init(void)
{
    ESP_LOGI(TAG, "Bluetooth disabled in this build");
    return ESP_OK;
}

bool sensor_hub_available(void)
{
    return false;
}

uint32_t sensor_hub_get_epoch(void)
{
    return 0;
}

esp_err_t sensor_hub_start_scan(sensor_kind_t kind)
{
    (void)kind;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sensor_hub_stop_scan(void)
{
    return ESP_OK;
}

bool sensor_hub_is_scanning(sensor_kind_t *kind_out)
{
    (void)kind_out;
    return false;
}

size_t sensor_hub_get_results(sensor_hub_device_t *out, size_t max)
{
    (void)out;
    (void)max;
    return 0;
}

esp_err_t sensor_hub_connect_result(size_t index)
{
    (void)index;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sensor_hub_connect_saved(sensor_kind_t kind)
{
    (void)kind;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sensor_hub_disconnect(sensor_kind_t kind)
{
    (void)kind;
    return ESP_OK;
}

esp_err_t sensor_hub_forget(sensor_kind_t kind)
{
    (void)kind;
    return ESP_OK;
}

void sensor_hub_get_slot(sensor_kind_t kind, sensor_hub_slot_info_t *out)
{
    (void)kind;
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = SENSOR_HUB_DISABLED;
    out->battery_pct = 255;
}

void sensor_hub_get_live(sensor_hub_live_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
}

#endif
