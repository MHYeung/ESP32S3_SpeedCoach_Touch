#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One connection slot per sensor kind. Values double as NVS slot indices. */
typedef enum {
    SENSOR_KIND_HR = 0,
    SENSOR_KIND_ROWPOD,
    SENSOR_KIND_COUNT,
} sensor_kind_t;

typedef enum {
    SENSOR_HUB_DISABLED = 0,
    SENSOR_HUB_IDLE,
    SENSOR_HUB_SCANNING,
    SENSOR_HUB_CONNECTING,
    SENSOR_HUB_CONNECTED,
} sensor_hub_state_t;

typedef struct {
    char name[24];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    sensor_kind_t kind;
} sensor_hub_device_t;

typedef struct {
    sensor_hub_state_t state;   /* SCANNING when a scan for this kind is running */
    bool saved;
    bool paused;                /* user disconnected; auto-reconnect held off */
    char name[24];
    uint8_t battery_pct;        /* 255 = unknown */
} sensor_hub_slot_info_t;

/* Live values for stroke_task. HR is 0 when stale (>5 s), off-skin or unpaired. */
typedef struct {
    uint16_t hr_bpm;
    bool hr_saved;
    bool hr_paused;             /* user tapped Disconnect; auto-reconnect is held */
    bool hr_connected;
    bool pod_connected;
    bool pod_valid;             /* a completed stroke has arrived */
    int16_t pod_catch_ddeg;     /* 0.1 deg */
    int16_t pod_finish_ddeg;
    uint16_t pod_arc_ddeg;
    uint16_t pod_spm_x10;
} sensor_hub_live_t;

esp_err_t sensor_hub_init(void);
bool sensor_hub_available(void);
uint32_t sensor_hub_get_epoch(void);

esp_err_t sensor_hub_start_scan(sensor_kind_t kind);
esp_err_t sensor_hub_stop_scan(void);
bool sensor_hub_is_scanning(sensor_kind_t *kind_out);
size_t sensor_hub_get_results(sensor_hub_device_t *out, size_t max);

esp_err_t sensor_hub_connect_result(size_t index);
esp_err_t sensor_hub_connect_saved(sensor_kind_t kind);
esp_err_t sensor_hub_disconnect(sensor_kind_t kind);
esp_err_t sensor_hub_forget(sensor_kind_t kind);

void sensor_hub_get_slot(sensor_kind_t kind, sensor_hub_slot_info_t *out);
void sensor_hub_get_live(sensor_hub_live_t *out);

/** Ask a connected RowPod to zero the oar (held square) or pick a side. */
esp_err_t sensor_hub_rowpod_zero(void);
esp_err_t sensor_hub_rowpod_set_side(bool bow);

#ifdef __cplusplus
}
#endif
