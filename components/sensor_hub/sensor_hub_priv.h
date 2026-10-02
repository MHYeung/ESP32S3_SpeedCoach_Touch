#pragma once

#include "sensor_hub.h"

#include "host/ble_hs.h"

#include <stdint.h>

#define SENSOR_HUB_MAX_RESULTS 6
#define SENSOR_NAME_LEN        24

typedef struct sensor_slot {
    sensor_kind_t kind;
    sensor_hub_state_t state;
    bool saved;
    bool paused;
    char name[SENSOR_NAME_LEN];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t conn_handle;
    uint8_t battery_pct;
    uint16_t hr_bpm;
    int64_t hr_us;
    uint32_t backoff_ms;
    uint16_t hrs_start;
    uint16_t hrs_end;
    uint16_t hrs_val_handle;
    uint8_t hrs_props;
    bool hrs_cccd_armed;
    uint16_t batt_start;
    uint16_t batt_end;
    uint16_t batt_val_handle;
    bool pod_valid;
    int16_t pod_catch_ddeg;
    int16_t pod_finish_ddeg;
    uint16_t pod_arc_ddeg;
    uint16_t pod_spm_x10;
} sensor_slot_t;

void sensor_hub_hrs_on_connect(sensor_slot_t *slot);
void sensor_hub_hrs_on_notify(sensor_slot_t *slot, struct os_mbuf *om);
void sensor_hub_rowpod_on_connect(sensor_slot_t *slot);
void sensor_hub_rowpod_on_notify(sensor_slot_t *slot, uint16_t attr, struct os_mbuf *om);
uint16_t sensor_hub_slot_conn(sensor_kind_t kind);
void sensor_hub_slot_set_hr(sensor_slot_t *slot, uint16_t bpm);
void sensor_hub_slot_set_battery(sensor_slot_t *slot, uint8_t pct);
void sensor_hub_slot_set_pod(sensor_slot_t *slot, bool valid, int16_t catch_ddeg,
                             int16_t finish_ddeg, uint16_t arc_ddeg, uint16_t spm_x10);
void sensor_hub_slot_drop(sensor_slot_t *slot);
void sensor_hub_hrs_subscribed(sensor_slot_t *slot);
