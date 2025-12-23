#include "pwr_key.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <limits.h>

static pwr_key_config_t s_cfg;
static pwr_key_cb_t s_cb = NULL;
static void *s_user = NULL;

static bool s_hold_state = false;

static inline bool key_is_pressed_raw(void)
{
    int lvl = gpio_get_level(s_cfg.key_gpio);
    return s_cfg.key_active_low ? (lvl == 0) : (lvl != 0);
}

void pwr_key_set_hold(bool on)
{
    s_hold_state = on;
    gpio_set_level(s_cfg.hold_gpio, on ? 1 : 0);
}

bool pwr_key_get_hold(void)
{
    return s_hold_state;
}

static void pwr_key_task(void *arg)
{
    (void)arg;

    bool last_raw = false;
    bool debounced = false;
    int64_t last_change_ms = 0;

    bool pressed = false;
    int64_t press_start_ms = -1;

    bool prompt_fired = false;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        bool raw = key_is_pressed_raw();
        if (raw != last_raw) {
            last_raw = raw;
            last_change_ms = now_ms;
        }

        if ((now_ms - last_change_ms) >= (int64_t)s_cfg.debounce_ms) {
            debounced = raw;
        }

        // Debounced press edge
        if (debounced && !pressed) {
            pressed = true;
            press_start_ms = now_ms;
            prompt_fired = false;
        }

        // While pressed, fire the 10s prompt once when threshold is reached
        if (pressed && press_start_ms >= 0 && !prompt_fired) {
            int64_t held = now_ms - press_start_ms;
            if ((uint32_t)held >= s_cfg.prompt_hold_ms) {
                prompt_fired = true;
                if (s_cb) s_cb(PWR_KEY_EVT_SHUTDOWN_PROMPT, s_user);
                // Do NOT auto-shutdown here; UI will ask user.
            }
        }

        // Debounced release edge
        if (!debounced && pressed) {
            pressed = false;
            int64_t held = (press_start_ms >= 0) ? (now_ms - press_start_ms) : 0;

            if (!prompt_fired) {
                if ((uint32_t)held >= s_cfg.toggle_hold_ms) {
                    if (s_cb) s_cb(PWR_KEY_EVT_ACTIVITY_TOGGLE, s_user);
                } else {
                    if (s_cb) s_cb(PWR_KEY_EVT_SHORT_PRESS, s_user);
                }
            }

            press_start_ms = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(s_cfg.poll_ms));
    }
}

esp_err_t pwr_key_init(const pwr_key_config_t *cfg, pwr_key_cb_t cb, void *user)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;
    s_cb = cb;
    s_user = user;

    if (s_cfg.debounce_ms == 0) s_cfg.debounce_ms = 30;
    if (s_cfg.poll_ms == 0) s_cfg.poll_ms = 20;
    if (s_cfg.toggle_hold_ms == 0) s_cfg.toggle_hold_ms = 2000;
    if (s_cfg.prompt_hold_ms == 0) s_cfg.prompt_hold_ms = 5000;

    // KEY input
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << s_cfg.key_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = s_cfg.key_active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = s_cfg.key_active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    // HOLD output
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << s_cfg.hold_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    // You can set this true immediately if you want the device to stay on.
    // pwr_key_set_hold(true);

    xTaskCreate(pwr_key_task, "pwr_key", 3072, NULL, 10, NULL);
    return ESP_OK;
}
