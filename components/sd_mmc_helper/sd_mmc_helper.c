#include "sd_mmc_helper.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#define SD_TAG "sd_mmc_helper"

/* Waveshare board SDMMC pins (from SD_MMC.h / pinout) */
#define SD_PIN_CLK 14 // SD SCLK
#define SD_PIN_CMD 17 // SD CMD
#define SD_PIN_D0 16  // SD D0 (MISO)

/* Mount the card at mount_point, e.g. "/sdcard" */
esp_err_t sd_mmc_helper_mount(sd_mmc_helper_t *sd,
                              const char *mount_point)
{
    if (!sd || !mount_point)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (sd->mounted)
    {
        ESP_LOGW(SD_TAG, "SD already mounted at %s", sd->mount_point);
        return ESP_OK;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit mode
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = -1;
    slot_config.d2 = -1;
    slot_config.d3 = -1;

    // Use internal pull-ups in addition to external ones, if present
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // OK for dev; turn off for production
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;

    ESP_LOGI(SD_TAG, "Mounting SD card at %s", mount_point);
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point,
                                            &host,
                                            &slot_config,
                                            &mount_config,
                                            &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(SD_TAG, "esp_vfs_fat_sdmmc_mount failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    sd->mounted = true;
    sd->card = card;
    sd->mount_point = mount_point;

    // Optional: print card info
    sdmmc_card_print_info(stdout, card);

    ESP_LOGI(SD_TAG, "SD card mounted OK");
    return ESP_OK;
}

esp_err_t sd_mmc_helper_unmount(sd_mmc_helper_t *sd)
{
    if (!sd || !sd->mounted)
    {
        return ESP_OK;
    }

    ESP_LOGI(SD_TAG, "Unmounting SD card from %s", sd->mount_point);
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(sd->mount_point, sd->card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(SD_TAG, "esp_vfs_fat_sdcard_unmount failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    sd->mounted = false;
    sd->card = NULL;
    sd->mount_point = NULL;

    return ESP_OK;
}

esp_err_t sd_mmc_helper_write_text(sd_mmc_helper_t *sd,
                                   const char *relative_path,
                                   const char *data,
                                   bool append)
{
    if (!sd || !sd->mounted)
    {
        ESP_LOGE(SD_TAG, "Cannot write: SD not mounted");
        return ESP_FAIL;
    }
    if (!relative_path || !data)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Build full path: "<mount_point>/<relative_path>" */
    char full_path[128];
    int n = snprintf(full_path, sizeof(full_path), "%s/%s",
                     sd->mount_point, relative_path);
    if (n <= 0 || n >= (int)sizeof(full_path))
    {
        ESP_LOGE(SD_TAG, "Path too long");
        return ESP_ERR_INVALID_ARG;
    }

    const char *mode = append ? "a" : "w";
    ESP_LOGI(SD_TAG, "Opening %s (%s)", full_path, mode);

    FILE *f = fopen(full_path, mode);
    if (!f)
    {
        ESP_LOGE(SD_TAG,
                 "Failed to open %s for writing, errno=%d (%s)",
                 full_path, errno, strerror(errno));
        return ESP_FAIL;
    }

    size_t len = strlen(data);
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len)
    {
        ESP_LOGE(SD_TAG, "Short write: expected %u, wrote %u",
                 (unsigned)len, (unsigned)written);
        return ESP_FAIL;
    }

    ESP_LOGI(SD_TAG, "Wrote %u bytes to %s",
             (unsigned)len, full_path);
    return ESP_OK;
}
