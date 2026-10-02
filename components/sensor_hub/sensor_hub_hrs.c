#include "sensor_hub_priv.h"

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include <string.h>

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && defined(CONFIG_BT_NIMBLE_ENABLED)

static const char *TAG = "sensor_hrs";

#define UUID_HRS  0x180D
#define UUID_HRM  0x2A37
#define UUID_BATT 0x180F
#define UUID_BATT_LVL 0x2A19

/* Bluetooth GSS Heart Rate Measurement flags. */
#define HRM_FLAG_UINT16   0x01
#define HRM_FLAG_CONTACT  0x06
#define HRM_CONTACT_NONE  0x02

static bool slot_live(const sensor_slot_t *slot, uint16_t conn)
{
    return slot && slot->conn_handle == conn && slot->state == SENSOR_HUB_CONNECTED;
}

static void fail(sensor_slot_t *slot, const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    sensor_hub_slot_drop(slot);
}

static void request_conn_params(uint16_t conn)
{
    /* 100–200 ms interval, latency 4: the strap notifies ~1 Hz, so it may
     * skip four events. Supervision 4 s is above (1+4)*200 ms*2. */
    struct ble_gap_upd_params params = {
        .itvl_min = 80,
        .itvl_max = 160,
        .latency = 4,
        .supervision_timeout = 400,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "conn param update rc=%d", rc);
    }
}

static int on_batt_read(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    sensor_slot_t *slot = arg;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && attr && attr->om) {
        uint8_t pct = 0;
        if (os_mbuf_copydata(attr->om, 0, 1, &pct) == 0 && pct <= 100) {
            sensor_hub_slot_set_battery(slot, pct);
            ESP_LOGI(TAG, "battery %u%%", (unsigned)pct);
        }
    }
    return 0;
}

static int on_batt_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    sensor_slot_t *slot = arg;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && chr) {
        slot->batt_val_handle = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (slot->batt_val_handle != 0) {
            int rc = ble_gattc_read(conn, slot->batt_val_handle, on_batt_read, slot);
            if (rc != 0) {
                ESP_LOGW(TAG, "battery read rc=%d", rc);
            }
        }
        return 0;
    }
    return 0;
}

static int on_batt_svc(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    sensor_slot_t *slot = arg;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && svc) {
        slot->batt_start = svc->start_handle;
        slot->batt_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (slot->batt_start == 0) {
            return 0;
        }
        int rc = ble_gattc_disc_chrs_by_uuid(conn, slot->batt_start, slot->batt_end,
                                             BLE_UUID16_DECLARE(UUID_BATT_LVL),
                                             on_batt_chr, slot);
        if (rc != 0) {
            ESP_LOGW(TAG, "battery chr rc=%d", rc);
        }
        return 0;
    }
    return 0;
}

static void start_battery(sensor_slot_t *slot)
{
    slot->batt_start = 0;
    slot->batt_end = 0;
    slot->batt_val_handle = 0;
    int rc = ble_gattc_disc_svc_by_uuid(slot->conn_handle, BLE_UUID16_DECLARE(UUID_BATT),
                                        on_batt_svc, slot);
    if (rc != 0) {
        ESP_LOGW(TAG, "battery svc rc=%d", rc);
    }
}

static int on_cccd_write(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    sensor_slot_t *slot = arg;
    (void)attr;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status != 0) {
        fail(slot, "HR CCCD write failed");
        return 0;
    }
    ESP_LOGI(TAG, "HR notifications on");
    sensor_hub_hrs_subscribed(slot);
    request_conn_params(conn);
    start_battery(slot);
    return 0;
}

static int on_hrs_dsc(uint16_t conn, const struct ble_gatt_error *error,
                      uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    sensor_slot_t *slot = arg;
    (void)chr_val_handle;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && dsc) {
        if (!slot->hrs_cccd_armed &&
            ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            uint8_t value[2] = {1, 0};
            if ((slot->hrs_props & BLE_GATT_CHR_PROP_NOTIFY) == 0 &&
                (slot->hrs_props & BLE_GATT_CHR_PROP_INDICATE) != 0) {
                value[0] = 2;
            }
            slot->hrs_cccd_armed = true;
            int rc = ble_gattc_write_flat(conn, dsc->handle, value, sizeof(value),
                                          on_cccd_write, slot);
            if (rc != 0) {
                fail(slot, "HR CCCD write start failed");
            }
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (!slot->hrs_cccd_armed) {
            fail(slot, "HR measurement has no CCCD");
        }
        return 0;
    }
    fail(slot, "HR descriptor discovery failed");
    return 0;
}

static int on_hrs_chr(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_chr *chr, void *arg)
{
    sensor_slot_t *slot = arg;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && chr) {
        slot->hrs_val_handle = chr->val_handle;
        slot->hrs_props = chr->properties;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (slot->hrs_val_handle == 0 || slot->hrs_end < slot->hrs_val_handle) {
            fail(slot, "HR measurement characteristic missing");
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn, slot->hrs_val_handle, slot->hrs_end,
                                         on_hrs_dsc, slot);
        if (rc != 0) {
            fail(slot, "HR descriptor discovery start failed");
        }
        return 0;
    }
    fail(slot, "HR characteristic discovery failed");
    return 0;
}

static int on_hrs_svc(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    sensor_slot_t *slot = arg;
    if (!slot_live(slot, conn)) {
        return 0;
    }
    if (error->status == 0 && svc) {
        slot->hrs_start = svc->start_handle;
        slot->hrs_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (slot->hrs_start == 0) {
            fail(slot, "heart rate service missing");
            return 0;
        }
        int rc = ble_gattc_disc_chrs_by_uuid(conn, slot->hrs_start, slot->hrs_end,
                                             BLE_UUID16_DECLARE(UUID_HRM),
                                             on_hrs_chr, slot);
        if (rc != 0) {
            fail(slot, "HR characteristic discovery start failed");
        }
        return 0;
    }
    fail(slot, "heart rate service discovery failed");
    return 0;
}

void sensor_hub_hrs_on_connect(sensor_slot_t *slot)
{
    if (!slot) {
        return;
    }
    slot->hrs_start = 0;
    slot->hrs_end = 0;
    slot->hrs_val_handle = 0;
    slot->hrs_props = 0;
    slot->hrs_cccd_armed = false;
    slot->batt_start = 0;
    slot->batt_val_handle = 0;
    slot->hr_bpm = 0;
    slot->hr_us = 0;
    slot->battery_pct = 255;

    int rc = ble_gattc_disc_svc_by_uuid(slot->conn_handle, BLE_UUID16_DECLARE(UUID_HRS),
                                        on_hrs_svc, slot);
    if (rc != 0) {
        fail(slot, "HR service discovery start failed");
    }
}

void sensor_hub_hrs_on_notify(sensor_slot_t *slot, struct os_mbuf *om)
{
    uint8_t buf[20];
    if (!slot || !om) {
        return;
    }
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len < 2) {
        return;
    }
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    if (os_mbuf_copydata(om, 0, len, buf) != 0) {
        return;
    }

    uint8_t flags = buf[0];
    uint16_t bpm;
    if (flags & HRM_FLAG_UINT16) {
        if (len < 3) {
            return;
        }
        bpm = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
    } else {
        bpm = buf[1];
    }

    uint8_t contact = (uint8_t)((flags & HRM_FLAG_CONTACT) >> 1);
    if (contact == HRM_CONTACT_NONE) {
        bpm = 0;
    }
    if (bpm > 250) {
        return;
    }
    sensor_hub_slot_set_hr(slot, bpm);
}

#else

void sensor_hub_hrs_on_connect(sensor_slot_t *slot)
{
    (void)slot;
}

void sensor_hub_hrs_on_notify(sensor_slot_t *slot, struct os_mbuf *om)
{
    (void)slot;
    (void)om;
}

#endif
