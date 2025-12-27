#include "nvs_helper.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "nvs_helper";
static const char *NAMESPACE = "storage"; // Common namespace for all settings

/* --- Internal Helper to open NVS --- */
static esp_err_t open_storage(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    esp_err_t err = nvs_open(NAMESPACE, mode, handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS Open Failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* --- Initialization --- */
esp_err_t nvs_helper_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS Partition truncated/corrupt. Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* --- Activity ID Logic --- */
uint32_t nvs_helper_get_next_activity_id(void)
{
    nvs_handle_t handle;
    uint32_t act_id = 0;

    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        // Read existing (default to 0 if not found)
        if (nvs_get_u32(handle, "act_id", &act_id) != ESP_OK) {
            act_id = 0;
        }
        
        act_id++; // Increment for new session

        // Save back
        nvs_set_u32(handle, "act_id", act_id);
        nvs_commit(handle);
        nvs_close(handle);
    } else {
        return 1; // Fallback if NVS fails
    }

    ESP_LOGI(TAG, "Next Activity ID: %lu", (unsigned long)act_id);
    return act_id;
}

/* --- Getters & Setters --- */

bool nvs_helper_get_dark_mode(void)
{
    nvs_handle_t handle;
    uint8_t val = 0; // Default false
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "dark_mode", &val);
        nvs_close(handle);
    }
    return (val != 0);
}

void nvs_helper_set_dark_mode(bool enabled)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "dark_mode", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool nvs_helper_get_auto_rotate(void)
{
    nvs_handle_t handle;
    uint8_t val = 1; // Default true
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "auto_rot", &val);
        nvs_close(handle);
    }
    return (val != 0);
}

void nvs_helper_set_auto_rotate(bool enabled)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "auto_rot", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

uint32_t nvs_helper_get_split_len(void)
{
    nvs_handle_t handle;
    uint32_t val = 1000; // Default 1000m
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u32(handle, "split_len", &val);
        nvs_close(handle);
    }
    return val;
}

void nvs_helper_set_split_len(uint32_t len_m)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u32(handle, "split_len", len_m);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nvs_helper_set_orientation(uint8_t orient)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "orient", orient);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

uint8_t nvs_helper_get_orientation(void)
{
    nvs_handle_t handle;
    uint8_t val = 0; // Default 0 (Portrait)
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "orient", &val);
        nvs_close(handle);
    }
    return val;
}