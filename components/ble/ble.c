#include <string.h>
#include "esp_log.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble.h"

static const char *TAG = "ble_app";

static char s_dev_name[BLE_NAME_MAX_LEN] = "ESP32S3-BLE";

/* -------------------------------------------------------------------------- */
/* Local state                                                                */
/* -------------------------------------------------------------------------- */

static uint8_t s_own_addr_type;
static int s_conn_handle = 0; // 0 means “no connection” for us

typedef struct
{
    ble_device_t dev;
    bool used;
} ble_device_entry_t;

static ble_device_entry_t s_devices[BLE_MAX_DEVICES];
static int s_device_count = 0;

/* UI callbacks */
static ble_device_list_changed_cb_t s_devlist_cb = NULL;
static ble_connection_state_cb_t s_conn_state_cb = NULL;
static ble_rx_cb_t s_rx_cb = NULL;

/* Forward declarations */
static void ble_host_task(void *param);
static int ble_gap_event(struct ble_gap_event *event, void *arg);
static esp_err_t ble_scan_internal_start(void);
static esp_err_t ble_advertise_internal_start(void);

/* -------------------------------------------------------------------------- */
/* Device list helpers                                                        */
/* -------------------------------------------------------------------------- */

static void devices_clear(void)
{
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
}

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    if (a->type != b->type)
    {
        return false;
    }
    return (memcmp(a->val, b->val, 6) == 0);
}

static void devices_add(const struct ble_gap_disc_desc *disc,
                        const uint8_t *name, uint8_t name_len)
{
    // Check if already in list
    for (int i = 0; i < BLE_MAX_DEVICES; ++i)
    {
        if (!s_devices[i].used)
        {
            continue;
        }
        ble_addr_t existing = {
            .type = s_devices[i].dev.addr_type,
        };
        memcpy(existing.val, s_devices[i].dev.addr, 6);

        if (addr_equal(&existing, &disc->addr))
        {
            // Update RSSI and name if needed
            s_devices[i].dev.rssi = disc->rssi;
            if (name && name_len > 0)
            {
                uint8_t copy_len = name_len;
                if (copy_len >= BLE_NAME_MAX_LEN)
                {
                    copy_len = BLE_NAME_MAX_LEN - 1;
                }
                memcpy(s_devices[i].dev.name, name, copy_len);
                s_devices[i].dev.name[copy_len] = '\0';
            }
            if (s_devlist_cb)
            {
                s_devlist_cb();
            }
            return;
        }
    }

    // Add new device if there is space
    for (int i = 0; i < BLE_MAX_DEVICES; ++i)
    {
        if (!s_devices[i].used)
        {
            s_devices[i].used = true;

            s_devices[i].dev.addr_type = disc->addr.type;
            memcpy(s_devices[i].dev.addr, disc->addr.val, 6);
            s_devices[i].dev.rssi = disc->rssi;

            if (name && name_len > 0)
            {
                uint8_t copy_len = name_len;
                if (copy_len >= BLE_NAME_MAX_LEN)
                {
                    copy_len = BLE_NAME_MAX_LEN - 1;
                }
                memcpy(s_devices[i].dev.name, name, copy_len);
                s_devices[i].dev.name[copy_len] = '\0';
            }
            else
            {
                strncpy(s_devices[i].dev.name, "Unknown", BLE_NAME_MAX_LEN);
                s_devices[i].dev.name[BLE_NAME_MAX_LEN - 1] = '\0';
            }

            if (s_device_count < i + 1)
            {
                s_device_count = i + 1;
            }

            ESP_LOGI(TAG, "Found device %s, addr_type=%d, rssi=%d",
                     s_devices[i].dev.name,
                     s_devices[i].dev.addr_type,
                     s_devices[i].dev.rssi);

            if (s_devlist_cb)
            {
                s_devlist_cb();
            }
            return;
        }
    }

    ESP_LOGW(TAG, "Device list full, ignoring new device");
}

/* -------------------------------------------------------------------------- */
/* GAP event handler                                                          */
/* -------------------------------------------------------------------------- */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    int rc;

    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
    {
        const struct ble_gap_disc_desc *disc = &event->disc;
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));

        rc = ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data);
        if (rc != 0)
        {
            ESP_LOGW(TAG, "Failed to parse adv fields; rc=%d", rc);
            return 0;
        }

        const uint8_t *name = NULL;
        uint8_t name_len = 0;

        if (fields.name && fields.name_len > 0)
        {
            name = fields.name;
            name_len = fields.name_len;
        }

        devices_add(disc, name, name_len);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete; reason=%d", event->disc_complete.reason);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete; reason=%d", event->adv_complete.reason);
        return 0;
        
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            ESP_LOGI(TAG, "Connection established; handle=%d",
                     event->connect.conn_handle);
            s_conn_handle = event->connect.conn_handle;
            if (s_conn_state_cb)
            {
                s_conn_state_cb(true);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Connection failed; status=%d", event->connect.status);
            s_conn_handle = 0;
            if (s_conn_state_cb)
            {
                s_conn_state_cb(false);
            }
            // Optionally restart scanning
            ble_scan_internal_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        s_conn_handle = 0;
        if (s_conn_state_cb)
        {
            s_conn_state_cb(false);
        }
        // Restart scanning so the user can pick another device
        ble_scan_internal_start();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (s_rx_cb)
        {
            // Forward raw data to app (GATT client logic to be added later)
            if (event->notify_rx.om)
            {
                // Get flat pointer to mbuf data
                uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
                uint8_t buf[256];

                if (len > sizeof(buf))
                {
                    len = sizeof(buf);
                }

                os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
                s_rx_cb(buf, len);
            }
        }
        return 0;

    default:
        return 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Scanning                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t ble_scan_internal_start(void)
{
    if (!ble_hs_synced())
    {
        ESP_LOGW(TAG, "Cannot start scan: host not synced yet");
        return ESP_FAIL;
    }

    int rc;
    struct ble_gap_disc_params params;

    memset(&params, 0, sizeof(params));
    params.itvl = 0;   // use stack default
    params.window = 0; // use stack default
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0; // active scan (request scan responses)
    params.filter_duplicates = 1;

    devices_clear();

    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params,
                      ble_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error initiating GAP discovery; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scan started");
    return ESP_OK;
}

static esp_err_t ble_advertise_internal_start(void)
{
    if (!ble_hs_synced())
    {
        ESP_LOGW(TAG, "Cannot advertise: host not synced yet");
        return ESP_FAIL;
    }

    // If we were scanning as central, stop first (adv + scan can't run together)
    ble_stop_scan();

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    // Use our GAP device name in the advertising payload
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed; rc=%d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; // connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; // general discoverable

    rc = ble_gap_adv_start(s_own_addr_type,
                           NULL, // own address
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_gap_event, // reuse same event handler
                           NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "ble_gap_adv_start failed; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising as '%s'", s_dev_name);
    return ESP_OK;
}
/* -------------------------------------------------------------------------- */
/* NimBLE host callbacks & config                                             */
/* -------------------------------------------------------------------------- */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "NimBLE host synced");

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to infer address type; rc=%d", rc);
        return;
    }

    uint8_t addr_val[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);

    ESP_LOGI(TAG, "Own addr type=%d, addr=%02X:%02X:%02X:%02X:%02X:%02X",
             s_own_addr_type,
             addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    // After sync → start scanning
    ble_start_scan();
}

static void nimble_host_config_init(void)
{
    // Set host callbacks
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Security config (simple/no IO, bonding off by default)
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t ble_app_init(void)
{
    int rc;

    // Initialize NimBLE host stack
    rc = nimble_port_init();
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to init NimBLE; rc=%d", rc);
        return ESP_FAIL;
    }

    // Initialize GAP / GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_set_device_name(s_dev_name);

    // Configure host callbacks and store
    nimble_host_config_init();

    // Start NimBLE host task (runs ble_hs)
    nimble_port_freertos_init(ble_host_task);

    return ESP_OK;
}

esp_err_t ble_start_scan(void)
{
    return ble_scan_internal_start();
}

esp_err_t ble_stop_scan(void)
{
    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "Failed to cancel scan; rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Scan stopped");
    return ESP_OK;
}

int ble_get_device_count(void)
{
    return s_device_count;
}

bool ble_get_device(int index, ble_device_t *out)
{
    if (!out)
    {
        return false;
    }

    if (index < 0 || index >= BLE_MAX_DEVICES)
    {
        return false;
    }

    if (!s_devices[index].used)
    {
        return false;
    }

    *out = s_devices[index].dev;
    return true;
}

esp_err_t ble_connect_to_index(int index)
{
    if (s_conn_handle != 0)
    {
        ESP_LOGW(TAG, "Already connected (handle=%d); disconnect first", s_conn_handle);
        return ESP_FAIL;
    }

    ble_device_t dev;
    if (!ble_get_device(index, &dev))
    {
        ESP_LOGW(TAG, "Invalid device index %d", index);
        return ESP_ERR_INVALID_ARG;
    }

    ble_addr_t peer_addr = {
        .type = dev.addr_type,
    };
    memcpy(peer_addr.val, dev.addr, 6);

    struct ble_gap_conn_params conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    // Leave 0 to let stack use defaults, or customize here if you want.

    int rc = ble_gap_connect(s_own_addr_type, &peer_addr,
                             BLE_HS_FOREVER, &conn_params,
                             ble_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start connect; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to index %d (%s)", index, dev.name);
    return ESP_OK;
}

esp_err_t ble_disconnect(void)
{
    if (s_conn_handle == 0)
    {
        return ESP_OK;
    }

    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to terminate connection; rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_send(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    // Placeholder: you’ll add GATT write/notify logic here later.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_set_device_name(const char *name)
{
    if (!name)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(name, BLE_NAME_MAX_LEN - 1);
    memcpy(s_dev_name, name, len);
    s_dev_name[len] = '\0';

    int rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "Failed to set GAP device name; rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Device name set to '%s'", s_dev_name);
    return ESP_OK;
}

const char *ble_get_device_name(void)
{
    return s_dev_name;
}

esp_err_t ble_start_advertising(void)
{
    return ble_advertise_internal_start();
}

esp_err_t ble_stop_advertising(void)
{
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        ESP_LOGE(TAG, "ble_gap_adv_stop failed; rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Advertising stopped");
    return ESP_OK;
}
/* -------------------------------------------------------------------------- */
/* Callback registration                                                      */
/* -------------------------------------------------------------------------- */

void ble_register_device_list_callback(ble_device_list_changed_cb_t cb)
{
    s_devlist_cb = cb;
}

void ble_register_connection_state_callback(ble_connection_state_cb_t cb)
{
    s_conn_state_cb = cb;
}

void ble_register_rx_callback(ble_rx_cb_t cb)
{
    s_rx_cb = cb;
}

/* -------------------------------------------------------------------------- */
/* NimBLE host FreeRTOS task                                                  */
/* -------------------------------------------------------------------------- */

static void ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // This function never returns until nimble_port_stop()
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}
