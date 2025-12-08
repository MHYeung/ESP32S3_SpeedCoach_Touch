#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_DEVICES   20
#define BLE_NAME_MAX_LEN  32

typedef struct
{
    uint8_t addr[6];        // MAC address (LSB-first as NimBLE gives it)
    uint8_t addr_type;      // BLE_ADDR_TYPE_*
    char    name[BLE_NAME_MAX_LEN];
    int8_t  rssi;
} ble_device_t;

/* Callbacks for UI integration */
typedef void (*ble_device_list_changed_cb_t)(void);
typedef void (*ble_connection_state_cb_t)(bool connected);
typedef void (*ble_rx_cb_t)(const uint8_t *data, uint16_t len);

/**
 * @brief Initialize NimBLE host stack and start host task.
 *
 * Call this once from app_main(), after NVS init.
 */
esp_err_t ble_app_init(void);

/**
 * @brief Start scanning for BLE devices.
 *
 * If the host is not yet synced, this returns ESP_FAIL; however
 * ble_app_init() already starts a scan automatically after sync.
 */
esp_err_t ble_start_scan(void);

/**
 * @brief Stop scanning.
 */
esp_err_t ble_stop_scan(void);

/**
 * @brief Get current number of discovered devices.
 */
int ble_get_device_count(void);

/**
 * @brief Get device info by index (0..count-1).
 *
 * @return true if index valid and info filled, false otherwise.
 */
bool ble_get_device(int index, ble_device_t *out);

/**
 * @brief Connect to device at given index in the current list.
 *
 * @return ESP_OK on success, error otherwise.
 */
esp_err_t ble_connect_to_index(int index);

/**
 * @brief Disconnect current connection, if any.
 */
esp_err_t ble_disconnect(void);

/* Device name helpers */
esp_err_t    ble_set_device_name(const char *name);
const char * ble_get_device_name(void);

/**
 * @brief Start/stop peripheral advertising so phones (nRF Connect, etc.)
 *        can see and connect to this ESP32.
 */
esp_err_t ble_start_advertising(void);
esp_err_t ble_stop_advertising(void);

/**
 * @brief Send data to peer (placeholder for later GATT implementation).
 *
 * For now this is just a stub returning ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t ble_send(const uint8_t *data, uint16_t len);

/* UI callback registration */

void ble_register_device_list_callback(ble_device_list_changed_cb_t cb);
void ble_register_connection_state_callback(ble_connection_state_cb_t cb);
void ble_register_rx_callback(ble_rx_cb_t cb);

#ifdef __cplusplus
}
#endif
