#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi_connect.h"
#include "hermes_mqtt.h"
#include "hermes_ui.h"

static const char *TAG = "hermes";

static void on_down(const char *json, int len) {
  hermes_ui_on_down_json(json, len);
}

void app_main(void) {
  ESP_LOGI(TAG, "Hermes firmware starting");
  ESP_ERROR_CHECK(nvs_flash_init());

  /* Display + boot animation + menu first — don't wait on Wi-Fi. */
  ESP_ERROR_CHECK(hermes_ui_start());

  esp_err_t wifi = hermes_wifi_connect();
  if (wifi != ESP_OK) {
    ESP_LOGW(TAG, "Wi-Fi not connected yet — UI still usable");
  } else {
    ESP_ERROR_CHECK(hermes_mqtt_start(on_down));
  }
}
