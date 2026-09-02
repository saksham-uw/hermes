#include "hermes_settings.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "hermes_cfg";
static const char *NAMESPACE = "hermes";
static const char *KEY_THEME = "theme";
static const char *KEY_BRIGHTNESS = "brightness";

static uint8_t valid_brightness(uint8_t brightness) {
  if (brightness < 5)
    return 5;
  if (brightness > 100)
    return 100;
  return brightness;
}

esp_err_t hermes_settings_load(hermes_theme_t *theme, uint8_t *brightness) {
  if (!theme || !brightness)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "no saved display settings yet");
    return err;
  }

  uint8_t stored_theme = (uint8_t)*theme;
  uint8_t stored_brightness = *brightness;
  esp_err_t theme_err = nvs_get_u8(handle, KEY_THEME, &stored_theme);
  esp_err_t brightness_err =
      nvs_get_u8(handle, KEY_BRIGHTNESS, &stored_brightness);
  nvs_close(handle);

  if (theme_err == ESP_OK && stored_theme < THEME_COUNT)
    *theme = (hermes_theme_t)stored_theme;
  if (brightness_err == ESP_OK)
    *brightness = valid_brightness(stored_brightness);

  ESP_LOGI(TAG, "loaded theme=%s brightness=%u from NVS",
           hermes_theme_name(*theme), (unsigned)*brightness);
  return theme_err == ESP_OK || brightness_err == ESP_OK ? ESP_OK
                                                         : ESP_ERR_NOT_FOUND;
}

esp_err_t hermes_settings_save(hermes_theme_t theme, uint8_t brightness) {
  if (theme >= THEME_COUNT)
    theme = THEME_NIGHT;
  brightness = valid_brightness(brightness);

  nvs_handle_t handle;
  esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK)
    return err;

  err = nvs_set_u8(handle, KEY_THEME, (uint8_t)theme);
  if (err == ESP_OK)
    err = nvs_set_u8(handle, KEY_BRIGHTNESS, brightness);
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "saved theme=%s brightness=%u to NVS",
             hermes_theme_name(theme), (unsigned)brightness);
  } else {
    ESP_LOGW(TAG, "could not save display settings: %s", esp_err_to_name(err));
  }
  return err;
}
