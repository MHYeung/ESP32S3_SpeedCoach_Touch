#include "sensor_hub_priv.h"

#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include <string.h>

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && defined(CONFIG_BT_NIMBLE_ENABLED)

static const char *TAG = "sensor_pod";

/* 9b7e1000 motion service, 9b7e1005 summary, 9b7e2000 config, 9b7e200c control. */
static const ble_uuid128_t s_uuid_motion =
    BLE_UUID128_INIT(0x00, 0x4f, 0xd5, 0xe6, 0xb2, 0x4b, 0x86, 0x9b,
                     0x71, 0x4f, 0x2f, 0x2b, 0x00, 0x10, 0x7e, 0x9b);
static const ble_uuid128_t s_uuid_summary =
    BLE_UUID128_INIT(0x00, 0x4f, 0xd5, 0xe6, 0xb2, 0x4b, 0x86, 0x9b,
                     0x71, 0x4f, 0x2f, 0x2b, 0x05, 0x10, 0x7e, 0x9b);
static const ble_uuid128_t s_uuid_cfg =
    BLE_UUID128_INIT(0x00, 0x4f, 0xd5, 0xe6, 0xb2, 0x4b, 0x86, 0x9b,
                     0x71, 0x4f, 0x2f, 0x2b, 0x00, 0x20, 0x7e, 0x9b);
static const ble_uuid128_t s_uuid_ctrl =
    BLE_UUID128_INIT(0x00, 0x4f, 0xd5, 0xe6, 0xb2, 0x4b, 0x86, 0x9b,
                     0x71, 0x4f, 0x2f, 0x2b, 0x0c, 0x20, 0x7e, 0x9b);

#define SUMMARY_LEN 20
#define FLAG_VALID  0x04

static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_summary_handle;
static uint16_t s_ctrl_handle;
static uint16_t s_cfg_start;
static uint16_t s_cfg_end;
static bool s_cccd_armed;
static sensor_slot_t *s_slot;

static bool slot_live(uint16_t conn)
{
    return s_slot && s_slot->conn_handle == conn && s_slot->state == SENSOR_HUB_CONNECTED;
}

static void request_conn_params(uint16_t conn)
{
    struct ble_gap_upd_params params = {
        .itvl_min = 24, /* 30 ms */
        .itvl_max = 40, /* 50 ms */
        .latency = 0,
        .supervision_timeout = 400,
    };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "conn param update rc=%d", rc);
    }
}

static int on_ctrl_chr(uint16_t conn, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status == 0 && chr) {
        s_ctrl_handle = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && s_ctrl_handle == 0) {
        ESP_LOGW(TAG, "pod has no coach-control characteristic");
    }
    return 0;
}

static int on_cfg_svc(uint16_t conn, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status == 0 && svc) {
        s_cfg_start = svc->start_handle;
        s_cfg_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE && s_cfg_start != 0) {
        int rc = ble_gattc_disc_chrs_by_uuid(conn, s_cfg_start, s_cfg_end,
                                             &s_uuid_ctrl.u, on_ctrl_chr, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "control chr rc=%d", rc);
        }
    }
    return 0;
}

static void start_control_discovery(uint16_t conn)
{
    s_ctrl_handle = 0;
    s_cfg_start = 0;
    s_cfg_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(conn, &s_uuid_cfg.u, on_cfg_svc, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "config svc rc=%d", rc);
    }
}

static int on_cccd_write(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGW(TAG, "summary CCCD status=%u", (unsigned)error->status);
        return 0;
    }
    ESP_LOGI(TAG, "RowPod stroke summary on");
    request_conn_params(conn);
    start_control_discovery(conn);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status == 0 && dsc && !s_cccd_armed &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        uint8_t value[2] = {1, 0};
        s_cccd_armed = true;
        int rc = ble_gattc_write_flat(conn, dsc->handle, value, sizeof(value),
                                      on_cccd_write, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD write rc=%d", rc);
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE && !s_cccd_armed) {
        ESP_LOGW(TAG, "summary characteristic has no CCCD; staying connected");
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status == 0 && chr) {
        s_summary_handle = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_summary_handle == 0 || s_svc_end < s_summary_handle) {
            ESP_LOGW(TAG, "pod firmware has no stroke summary; GAP link only");
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn, s_summary_handle, s_svc_end, on_dsc, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "descriptor discovery rc=%d", rc);
        }
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (!slot_live(conn)) {
        return 0;
    }
    if (error->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start == 0) {
            ESP_LOGW(TAG, "motion service missing");
            return 0;
        }
        int rc = ble_gattc_disc_chrs_by_uuid(conn, s_svc_start, s_svc_end,
                                             &s_uuid_summary.u, on_chr, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "summary chr rc=%d", rc);
        }
    }
    return 0;
}

void sensor_hub_rowpod_on_connect(sensor_slot_t *slot)
{
    s_slot = slot;
    s_svc_start = 0;
    s_svc_end = 0;
    s_summary_handle = 0;
    s_ctrl_handle = 0;
    s_cccd_armed = false;
    if (slot) {
        slot->pod_valid = false;
    }
    int rc = ble_gattc_disc_svc_by_uuid(slot->conn_handle, &s_uuid_motion.u, on_svc, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "motion svc rc=%d", rc);
    }
}

void sensor_hub_rowpod_on_notify(sensor_slot_t *slot, uint16_t attr, struct os_mbuf *om)
{
    uint8_t buf[SUMMARY_LEN];
    if (!slot || !om || attr != s_summary_handle) {
        return;
    }
    if (OS_MBUF_PKTLEN(om) < SUMMARY_LEN) {
        return;
    }
    if (os_mbuf_copydata(om, 0, SUMMARY_LEN, buf) != 0) {
        return;
    }
    uint8_t flags = buf[1];
    bool valid = (flags & FLAG_VALID) != 0;
    int16_t catch_d = (int16_t)(buf[8] | (buf[9] << 8));
    int16_t finish_d = (int16_t)(buf[10] | (buf[11] << 8));
    uint16_t arc = (uint16_t)(buf[12] | (buf[13] << 8));
    uint16_t spm = (uint16_t)(buf[14] | (buf[15] << 8));
    uint8_t batt = buf[18];
    if (batt <= 100) {
        sensor_hub_slot_set_battery(slot, batt);
    }
    sensor_hub_slot_set_pod(slot, valid, catch_d, finish_d, arc, spm);
    ESP_LOGI(TAG, "stroke catch=%.1f finish=%.1f arc=%.1f spm=%.1f",
             (double)catch_d / 10.0, (double)finish_d / 10.0,
             (double)arc / 10.0, (double)spm / 10.0);
}

static esp_err_t write_ctrl(const uint8_t *data, uint8_t len)
{
    uint16_t conn = sensor_hub_slot_conn(SENSOR_KIND_ROWPOD);
    if (conn == BLE_HS_CONN_HANDLE_NONE || s_ctrl_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = ble_gattc_write_flat(conn, s_ctrl_handle, data, len, NULL, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sensor_hub_rowpod_zero(void)
{
    uint8_t op = 1;
    return write_ctrl(&op, 1);
}

esp_err_t sensor_hub_rowpod_set_side(bool bow)
{
    uint8_t buf[2] = {2, bow ? 1 : 0};
    return write_ctrl(buf, 2);
}

#else

void sensor_hub_rowpod_on_connect(sensor_slot_t *slot)
{
    (void)slot;
}

void sensor_hub_rowpod_on_notify(sensor_slot_t *slot, uint16_t attr, struct os_mbuf *om)
{
    (void)slot;
    (void)attr;
    (void)om;
}

esp_err_t sensor_hub_rowpod_zero(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sensor_hub_rowpod_set_side(bool bow)
{
    (void)bow;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
