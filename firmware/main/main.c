#include "esp_log.h"
#include "esp_system.h"
#include "hermes_ui.h"
#include "home_dashboard.h"
#include "nvs_flash.h"
#include "wifi_connect.h"

static const char *TAG = "hermes";

static const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "power-on";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt-watchdog";
  case ESP_RST_TASK_WDT:
    return "task-watchdog";
  case ESP_RST_WDT:
    return "watchdog";
  case ESP_RST_DEEPSLEEP:
    return "deep-sleep wake";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_USB:
    return "USB peripheral";
  case ESP_RST_JTAG:
    return "JTAG";
  case ESP_RST_PWR_GLITCH:
    return "power glitch";
  default:
    return "other";
  }
}

void app_main(void) {
  esp_reset_reason_t reset_reason = esp_reset_reason();
  ESP_LOGI(TAG, "Hermes firmware starting; reset=%s (%d)",
           reset_reason_name(reset_reason), (int)reset_reason);
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs);

  /* The standalone interface boots immediately; Wi-Fi associates in the
   * background and is managed entirely from the device. */
  ESP_ERROR_CHECK(hermes_ui_start());
  esp_err_t wifi = hermes_wifi_start();
  if (wifi != ESP_OK)
    ESP_LOGW(TAG, "Wi-Fi startup deferred: %s", esp_err_to_name(wifi));
  else
    home_dashboard_start_network_time();
}
