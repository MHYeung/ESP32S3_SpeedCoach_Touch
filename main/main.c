#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "activity.h"
#include "activity_log.h"
#include "gps_gtu8.h"
#include "nvs_helper.h"
#include "sensor_hub.h"
#include "rtc_pcf85063.h"
#include "sd_mmc_helper.h"

#include "ui.h"
#include "ui_data_page.h"
#include "ui_settings_page.h"

#include "app/app_activity.h"
#include "app/app_context.h"
#include "app/app_display.h"
#include "app/app_imu.h"
#include "app/app_power.h"
#include "app/app_settings.h"
#include "app/app_time.h"
#include "app/app_usb_msc.h"

static const char *TAG = "app";

#define LOG_QUEUE_LEN 32

void app_main(void)
{
    init_display_and_lvgl();
    init_touch_and_lvgl_input();
    init_imu();
    PCF85063_init(&s_imu_bus);
    ESP_ERROR_CHECK(nvs_helper_init());
    ESP_ERROR_CHECK(sensor_hub_init());

    gps_gtu8_config_t gps_cfg = {
        .uart_num = UART_NUM_1,
        .tx_gpio = 43, // board TXD
        .rx_gpio = 44, // board RXD
        .baud = 9600,  // common GT-U8 default
        .task_prio = 8,
        .task_stack = 4096,
        .rx_buf_size = 2048,
    };
    ESP_ERROR_CHECK(gps_gtu8_init(&gps_cfg));
    ESP_ERROR_CHECK(gps_gtu8_set_callback(gps_fix_cb, NULL));

    app_set_time_from_rtc();

    /* Activity queue must exist before pwr_key_task can post START/STOP.
     * ui_init() is heavy; a PWR press during it used to hit xQueueSend(NULL). */
    s_activity_mutex = xSemaphoreCreateMutex();
    activity_init(&s_activity, 0);
    s_session_time_s = 0.0f;
    s_session_last_us = esp_timer_get_time();

    s_act_q = xQueueCreate(4, sizeof(act_cmd_t));
    ESP_ERROR_CHECK(s_act_q ? ESP_OK : ESP_ERR_NO_MEM);
    s_log_q = xQueueCreate(LOG_QUEUE_LEN, sizeof(activity_log_msg_t));
    ESP_ERROR_CHECK(s_log_q ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(activity_logger_task, "activity_logger", 6144, NULL, 6, NULL);
    xTaskCreate(activity_worker_task, "activity_worker", 8192, NULL, 9, &s_act_worker_task);
    if (app_usb_msc_init() != ESP_OK)
        ESP_LOGE(TAG, "USB MSC worker not started");

    /* Create UI in separate module */
    ui_init(s_disp);

    /* Default Data page metrics: pace / SPM / distance (NVS override) */
    uint8_t saved_slots[3];
    nvs_helper_get_data_metrics(saved_slots);
    data_metric_t metrics[3];
    for (int i = 0; i < 3; i++) {
        metrics[i] = (saved_slots[i] < DATA_METRIC_COUNT)
                         ? (data_metric_t)saved_slots[i]
                         : DATA_METRIC_PACE;
    }
    data_page_set_metrics(metrics, 3);

    esp_err_t sd_err = sd_mmc_helper_mount(&s_sd, "/sdcard");
    if (sd_err != ESP_OK)
    {
        ESP_LOGW(TAG, "SD mount failed: %s (continuing)", esp_err_to_name(sd_err));
    }

    bool saved_dark = nvs_helper_get_dark_mode();
    ui_set_dark_mode(saved_dark);
    uint8_t saved_val = nvs_helper_get_orientation();
    ui_set_orientation((ui_orientation_t)saved_val);
    uint32_t saved_split = nvs_helper_get_split_len();
    s_current_split_m = saved_split;
    activity_log_init(&s_act_log); // Ensure log is init'd before setting interval
    activity_log_set_split_interval(&s_act_log, saved_split);

    ui_register_dark_mode_cb(on_dark_mode_setting_changed);
    ui_register_auto_rotate_cb(on_auto_rotate_setting_changed);
    ui_register_shutdown_confirm_cb(on_shutdown_confirmed);
    ui_register_stop_save_confirm_cb(on_stop_save_confirmed);
    ui_settings_register_split_length_cb(on_split_interval_changed);

    app_pwr_key_setup();

    /* Created last, so it competes for whatever internal DRAM is left after
     * LVGL/SD/BLE. A silent pdFAIL here kills SPM with no other symptom. */
    BaseType_t stroke_ok = xTaskCreatePinnedToCore(stroke_task, "stroke",
                                                   6144, NULL, 3, NULL, 0);
    if (stroke_ok != pdPASS)
    {
        ESP_LOGE(TAG, "stroke task create FAILED, free internal=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    else
    {
        ESP_LOGI(TAG, "stroke task started, free internal=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    /* app_main can idle */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
