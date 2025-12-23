#include "battery_drv.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery_drv";

struct battery_drv_s {
    battery_drv_config_t cfg;

    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool cali_enabled;

    SemaphoreHandle_t lock;
};

static bool adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    esp_err_t ret;
    bool calibrated = false;
    adc_cali_handle_t handle = NULL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "ADC calibration: curve fitting");
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
            ESP_LOGI(TAG, "ADC calibration: line fitting");
        }
    }
#endif

    *out_handle = handle;
    return calibrated;
}

static void adc_calibration_deinit(adc_cali_handle_t handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(handle);
#else
    (void)handle;
#endif
}

esp_err_t battery_drv_init(const battery_drv_config_t *cfg, battery_drv_handle_t *out)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;

    battery_drv_handle_t h = calloc(1, sizeof(*h));
    if (!h) return ESP_ERR_NO_MEM;

    h->cfg = *cfg;
    if (h->cfg.samples == 0) h->cfg.samples = 8;
    if (h->cfg.divider_ratio <= 0.0f) h->cfg.divider_ratio = 3.0f;
    if (h->cfg.measurement_offset <= 0.0f) h->cfg.measurement_offset = 1.0f;

    h->lock = xSemaphoreCreateMutex();
    if (!h->lock) {
        free(h);
        return ESP_ERR_NO_MEM;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = h->cfg.unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &h->adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        battery_drv_deinit(h);
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = h->cfg.atten,
        .bitwidth = h->cfg.bitwidth,
    };
    err = adc_oneshot_config_channel(h->adc_handle, h->cfg.channel, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        battery_drv_deinit(h);
        return err;
    }

    h->cali_enabled = adc_calibration_init(h->cfg.unit, h->cfg.atten, &h->cali_handle);
    if (!h->cali_enabled) {
        ESP_LOGW(TAG, "ADC calibration not available; voltage will be approximate");
    }

    *out = h;
    return ESP_OK;
}

void battery_drv_deinit(battery_drv_handle_t h)
{
    if (!h) return;

    if (h->cali_enabled && h->cali_handle) {
        adc_calibration_deinit(h->cali_handle);
        h->cali_handle = NULL;
        h->cali_enabled = false;
    }

    if (h->adc_handle) {
        adc_oneshot_del_unit(h->adc_handle);
        h->adc_handle = NULL;
    }

    if (h->lock) {
        vSemaphoreDelete(h->lock);
        h->lock = NULL;
    }

    free(h);
}

esp_err_t battery_drv_read_adc_mv(battery_drv_handle_t h, int *out_mv)
{
    if (!h || !out_mv) return ESP_ERR_INVALID_ARG;

    int raw = 0;
    int mv_sum = 0;

    xSemaphoreTake(h->lock, portMAX_DELAY);

    for (int i = 0; i < h->cfg.samples; i++) {
        esp_err_t err = adc_oneshot_read(h->adc_handle, h->cfg.channel, &raw);
        if (err != ESP_OK) {
            xSemaphoreGive(h->lock);
            return err;
        }

        if (h->cali_enabled) {
            int mv = 0;
            err = adc_cali_raw_to_voltage(h->cali_handle, raw, &mv);
            if (err != ESP_OK) {
                xSemaphoreGive(h->lock);
                return err;
            }
            mv_sum += mv;
        } else {
            // Without calibration, just return raw (not great). We approximate by scaling to 0..3300mV.
            // ESP-IDF doesn't provide a perfect raw->mV mapping without calibration.
            // Use a coarse fallback:
            mv_sum += (raw * 3300) / 4095;
        }
    }

    xSemaphoreGive(h->lock);

    *out_mv = mv_sum / h->cfg.samples;
    return ESP_OK;
}

esp_err_t battery_drv_read_battery_v(battery_drv_handle_t h, float *out_v)
{
    if (!h || !out_v) return ESP_ERR_INVALID_ARG;

    int adc_mv = 0;
    esp_err_t err = battery_drv_read_adc_mv(h, &adc_mv);
    if (err != ESP_OK) return err;

    float v_adc = (float)adc_mv / 1000.0f;
    float v_bat = (v_adc * h->cfg.divider_ratio) / h->cfg.measurement_offset;

    *out_v = v_bat;
    return ESP_OK;
}

esp_err_t battery_drv_read_percent(battery_drv_handle_t h, int *out_percent)
{
    if (!h || !out_percent) return ESP_ERR_INVALID_ARG;

    float v = 0.0f;
    esp_err_t err = battery_drv_read_battery_v(h, &v);
    if (err != ESP_OK) return err;

    float empty = h->cfg.v_empty;
    float full  = h->cfg.v_full;
    if (full <= empty) {
        empty = 3.3f;
        full  = 4.2f;
    }

    float pct_f = (v - empty) / (full - empty) * 100.0f;
    if (pct_f < 0.0f) pct_f = 0.0f;
    if (pct_f > 100.0f) pct_f = 100.0f;

    *out_percent = (int)(pct_f + 0.5f);
    return ESP_OK;
}
