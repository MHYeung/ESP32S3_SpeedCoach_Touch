#include "nvs_helper.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

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

void nvs_helper_get_data_metrics(uint8_t out[3])
{
    if (!out) {
        return;
    }
    /* Sport-neutral defaults: Pace, SPM, Distance */
    out[0] = 0; /* DATA_METRIC_PACE */
    out[1] = 5; /* DATA_METRIC_SPM */
    out[2] = 3; /* DATA_METRIC_DISTANCE */

    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READONLY) != ESP_OK) {
        return;
    }
    nvs_get_u8(handle, "slot0", &out[0]);
    nvs_get_u8(handle, "slot1", &out[1]);
    nvs_get_u8(handle, "slot2", &out[2]);
    nvs_close(handle);
}

void nvs_helper_set_data_metrics(const uint8_t in[3])
{
    if (!in) {
        return;
    }
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) != ESP_OK) {
        return;
    }
    nvs_set_u8(handle, "slot0", in[0]);
    nvs_set_u8(handle, "slot1", in[1]);
    nvs_set_u8(handle, "slot2", in[2]);
    nvs_commit(handle);
    nvs_close(handle);
}

bool nvs_helper_get_metrics_lock(void)
{
    nvs_handle_t handle;
    uint8_t val = 1;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "met_lock", &val);
        nvs_close(handle);
    }
    return (val != 0);
}

void nvs_helper_set_metrics_lock(bool enabled)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "met_lock", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

uint8_t nvs_helper_get_brightness(void)
{
    nvs_handle_t handle;
    uint8_t val = 80;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "bright", &val);
        nvs_close(handle);
    }
    if (val < 10) {
        val = 10;
    }
    if (val > 100) {
        val = 100;
    }
    return val;
}

void nvs_helper_set_brightness(uint8_t percent)
{
    if (percent < 10) {
        percent = 10;
    }
    if (percent > 100) {
        percent = 100;
    }
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "bright", percent);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool nvs_helper_get_auto_dim(void)
{
    nvs_handle_t handle;
    uint8_t val = 1;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "autodim", &val);
        nvs_close(handle);
    }
    return (val != 0);
}

void nvs_helper_set_auto_dim(bool enabled)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "autodim", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool nvs_helper_get_sensors_enabled(void)
{
    nvs_handle_t handle;
    uint8_t val = 0;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "sn_en", &val);
        nvs_close(handle);
    }
    return (val != 0);
}

void nvs_helper_set_sensors_enabled(bool enabled)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "sn_en", enabled ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nvs_helper_get_sensor_addr(uint8_t out[6])
{
    if (!out) {
        return;
    }
    memset(out, 0, 6);
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READONLY) != ESP_OK) {
        return;
    }
    size_t len = 6;
    nvs_get_blob(handle, "sn_addr", out, &len);
    nvs_close(handle);
}

void nvs_helper_set_sensor_addr(const uint8_t in[6])
{
    if (!in) {
        return;
    }
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, "sn_addr", in, 6);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t nvs_helper_get_sensor_addr_type(void)
{
    nvs_handle_t handle;
    uint8_t val = 0;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "sn_atyp", &val);
        nvs_close(handle);
    }
    return val;
}

void nvs_helper_set_sensor_addr_type(uint8_t addr_type)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "sn_atyp", addr_type);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void get_str_key(const char *key, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READONLY) != ESP_OK) {
        return;
    }
    size_t len = out_len;
    if (nvs_get_str(handle, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(handle);
}

static void set_str_key(const char *key, const char *name)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, key, name ? name : "");
    nvs_commit(handle);
    nvs_close(handle);
}

void nvs_helper_get_sensor_name(char *out, size_t out_len)
{
    get_str_key("sn_name", out, out_len);
}

void nvs_helper_set_sensor_name(const char *name)
{
    set_str_key("sn_name", name);
}

void nvs_helper_get_hr_addr(uint8_t out[6])
{
    if (!out) {
        return;
    }
    memset(out, 0, 6);
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READONLY) != ESP_OK) {
        return;
    }
    size_t len = 6;
    nvs_get_blob(handle, "hr_addr", out, &len);
    nvs_close(handle);
}

void nvs_helper_set_hr_addr(const uint8_t in[6])
{
    if (!in) {
        return;
    }
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, "hr_addr", in, 6);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t nvs_helper_get_hr_addr_type(void)
{
    nvs_handle_t handle;
    uint8_t val = 0;
    if (open_storage(&handle, NVS_READONLY) == ESP_OK) {
        nvs_get_u8(handle, "hr_atyp", &val);
        nvs_close(handle);
    }
    return val;
}

void nvs_helper_set_hr_addr_type(uint8_t addr_type)
{
    nvs_handle_t handle;
    if (open_storage(&handle, NVS_READWRITE) == ESP_OK) {
        nvs_set_u8(handle, "hr_atyp", addr_type);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void nvs_helper_get_hr_name(char *out, size_t out_len)
{
    get_str_key("hr_name", out, out_len);
}

void nvs_helper_set_hr_name(const char *name)
{
    set_str_key("hr_name", name);
}