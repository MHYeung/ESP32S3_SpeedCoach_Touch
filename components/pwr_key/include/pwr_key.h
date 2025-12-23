#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWR_KEY_EVT_SHORT_PRESS = 0,
    PWR_KEY_EVT_ACTIVITY_TOGGLE,          // released after >=2s and <10s
    PWR_KEY_EVT_SHUTDOWN_PROMPT,         // fired once when held reaches 10s
} pwr_key_event_t;

typedef void (*pwr_key_cb_t)(pwr_key_event_t evt, void *user);

typedef struct {
    gpio_num_t key_gpio;        // e.g. GPIO_NUM_6
    gpio_num_t hold_gpio;       // e.g. GPIO_NUM_7
    bool key_active_low;        // true if pressed==0

    uint32_t debounce_ms;       // e.g. 30
    uint32_t poll_ms;           // e.g. 20

    uint32_t toggle_hold_ms;     // 2000
    uint32_t prompt_hold_ms;    // 10000
} pwr_key_config_t;

esp_err_t pwr_key_init(const pwr_key_config_t *cfg, pwr_key_cb_t cb, void *user);

void pwr_key_set_hold(bool on);     // keep power latched on/off
bool pwr_key_get_hold(void);

#ifdef __cplusplus
}
#endif
