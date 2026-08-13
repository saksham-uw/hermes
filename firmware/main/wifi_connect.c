#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_connect.h"
#include "sdkconfig.h"

static const char *TAG = "hermes_wifi";
static EventGroupHandle_t s_wifi_events;
static const int WIFI_OK = BIT0;
static const int WIFI_FAIL = BIT1;
static int s_retry = 0;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry < 12) {
      esp_wifi_connect();
      s_retry++;
      ESP_LOGW(TAG, "retry %d", s_retry);
    } else {
      xEventGroupSetBits(s_wifi_events, WIFI_FAIL);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_retry = 0;
    xEventGroupSetBits(s_wifi_events, WIFI_OK);
  }
}

esp_err_t hermes_wifi_connect(void) {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, CONFIG_HERMES_WIFI_SSID, sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, CONFIG_HERMES_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_OK | WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
  if (bits & WIFI_OK) {
    ESP_LOGI(TAG, "connected");
    return ESP_OK;
  }
  ESP_LOGE(TAG, "failed to connect");
  return ESP_FAIL;
}
