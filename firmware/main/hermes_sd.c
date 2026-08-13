#include "hermes_sd.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "hermes_sd";
static sdmmc_card_t *s_card;
static bool s_ready;

esp_err_t hermes_sd_mount(void) {
  if (s_ready) return ESP_OK;

  esp_vfs_fat_sdmmc_mount_config_t mount = {
      .format_if_mount_failed = false,
      .max_files = 4,
      .allocation_unit_size = 16 * 1024,
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1; /* Waveshare 3.49 TF slot is 1-line SDMMC */
  slot.clk = HERMES_SD_CLK;
  slot.cmd = HERMES_SD_CMD;
  slot.d0 = HERMES_SD_D0;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t err =
      esp_vfs_fat_sdmmc_mount(HERMES_SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SD mount failed: %s (settings won't persist)", esp_err_to_name(err));
    s_card = NULL;
    s_ready = false;
    return err;
  }

  s_ready = true;
  ESP_LOGI(TAG, "mounted %s", HERMES_SD_MOUNT_POINT);
  sdmmc_card_print_info(stdout, s_card);
  return ESP_OK;
}

bool hermes_sd_ready(void) { return s_ready; }
