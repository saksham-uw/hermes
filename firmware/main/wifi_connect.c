#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "wifi_connect.h"

static const char *TAG = "hermes_wifi";

static const hermes_wifi_net_t k_nets[HERMES_WIFI_COUNT] = {
    {.ssid = "roger7459", .password = "Sharry22#%", .label = "home wifi"},
    {.ssid = "Saksham", .password = "yomamafatvery", .label = "personal hotspot"},
    {.ssid = "Adelaide_Club", .password = "4163679957", .label = "office wifi"},
};

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_lock;
static const int WIFI_OK = BIT0;
static const int WIFI_FAIL = BIT1;
static int s_retry = 0;
static int s_active = 0;
static bool s_started;
static bool s_connected;
static bool s_auto_reconnect = true;
static hermes_wifi_sight_t s_sight[HERMES_WIFI_COUNT];

static void apply_sta_config(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT) index = 0;
  s_active = index;
  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, k_nets[index].ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, k_nets[index].password,
          sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_LOGI(TAG, "using %s (%s)", k_nets[index].ssid, k_nets[index].label);
}

static void persist_active(void) {
  nvs_handle_t h;
  if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_i32(h, "wifi_idx", s_active);
  nvs_commit(h);
  nvs_close(h);
}

static int load_active(void) {
  nvs_handle_t h;
  int32_t v = 0;
  if (nvs_open("hermes", NVS_READONLY, &h) != ESP_OK) return 0;
  nvs_get_i32(h, "wifi_idx", &v);
  nvs_close(h);
  if (v < 0 || v >= HERMES_WIFI_COUNT) v = 0;
  return (int)v;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (s_auto_reconnect) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_connected = false;
    if (s_auto_reconnect && s_retry < 10) {
      esp_wifi_connect();
      s_retry++;
      ESP_LOGW(TAG, "retry %d on %s", s_retry, k_nets[s_active].ssid);
    } else if (!s_auto_reconnect || s_retry >= 10) {
      xEventGroupSetBits(s_wifi_events, WIFI_FAIL);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_retry = 0;
    s_connected = true;
    xEventGroupSetBits(s_wifi_events, WIFI_OK);
    ESP_LOGI(TAG, "connected %s", k_nets[s_active].ssid);
  }
}

bool hermes_wifi_has_internet(void) {
  if (!s_connected) return false;
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) return false;
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  struct sockaddr_in dest = {0};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(53);
  dest.sin_addr.s_addr = inet_addr("8.8.8.8");
  int rc = connect(sock, (struct sockaddr *)&dest, sizeof(dest));
  close(sock);
  return rc == 0;
}

esp_err_t hermes_wifi_start(void) {
  if (s_started) return ESP_OK;
  s_lock = xSemaphoreCreateMutex();
  s_wifi_events = xEventGroupCreate();
  s_active = load_active();
  memset(s_sight, 0, sizeof(s_sight));

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi, NULL));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  apply_sta_config(s_active);
  ESP_ERROR_CHECK(esp_wifi_start());
  s_started = true;
  return ESP_OK;
}

esp_err_t hermes_wifi_connect(void) {
  ESP_ERROR_CHECK(hermes_wifi_start());
  xEventGroupClearBits(s_wifi_events, WIFI_OK | WIFI_FAIL);
  s_retry = 0;
  s_auto_reconnect = true;
  esp_wifi_connect();
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_events, WIFI_OK | WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000));
  return (bits & WIFI_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t hermes_wifi_refresh_scan(void) {
  if (!s_started) hermes_wifi_start();
  memset(s_sight, 0, sizeof(s_sight));

  wifi_scan_config_t scan = {
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
  };
  /* Keep association; ESP32 can scan while connected. */
  esp_err_t err = esp_wifi_scan_start(&scan, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "scan failed %s", esp_err_to_name(err));
    return err;
  }
  uint16_t ap_num = 0;
  esp_wifi_scan_get_ap_num(&ap_num);
  if (ap_num > 40) ap_num = 40;
  wifi_ap_record_t recs[40];
  uint16_t got = ap_num;
  if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) return ESP_FAIL;

  for (uint16_t i = 0; i < got; i++) {
    for (int n = 0; n < HERMES_WIFI_COUNT; n++) {
      if (strcmp((const char *)recs[i].ssid, k_nets[n].ssid) != 0) continue;
      if (!s_sight[n].visible || recs[i].rssi > s_sight[n].rssi) {
        s_sight[n].visible = true;
        s_sight[n].rssi = recs[i].rssi;
        s_sight[n].channel = recs[i].primary;
      }
    }
  }
  /* Active connection always counts as visible. */
  if (s_connected) {
    s_sight[s_active].visible = true;
    int r = hermes_wifi_rssi();
    if (r != 0) s_sight[s_active].rssi = (int8_t)r;
  }
  return ESP_OK;
}

bool hermes_wifi_is_selectable(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT) return false;
  return s_sight[index].visible;
}

const hermes_wifi_sight_t *hermes_wifi_sight(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT) return NULL;
  return &s_sight[index];
}

static esp_err_t connect_wait(int timeout_ms) {
  xEventGroupClearBits(s_wifi_events, WIFI_OK | WIFI_FAIL);
  s_retry = 0;
  s_auto_reconnect = true;
  esp_wifi_connect();
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_events, WIFI_OK | WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t hermes_wifi_select(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT) return ESP_ERR_INVALID_ARG;
  if (!s_started) hermes_wifi_start();

  hermes_wifi_refresh_scan();
  if (!hermes_wifi_is_selectable(index)) {
    ESP_LOGW(TAG, "%s not in range — keeping current", k_nets[index].ssid);
    return ESP_ERR_NOT_FOUND;
  }
  if (index == s_active && s_connected && hermes_wifi_has_internet()) {
    return ESP_OK;
  }

  int prev = s_active;
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
  s_connected = false;
  s_auto_reconnect = false;
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(200));
  apply_sta_config(index);
  if (s_lock) xSemaphoreGive(s_lock);

  esp_err_t err = connect_wait(18000);
  bool ok = (err == ESP_OK) && hermes_wifi_has_internet();
  if (!ok) {
    ESP_LOGW(TAG, "%s has no internet — reverting to %s", k_nets[index].ssid, k_nets[prev].ssid);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connected = false;
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    apply_sta_config(prev);
    if (s_lock) xSemaphoreGive(s_lock);
    connect_wait(18000);
    return ESP_FAIL;
  }
  persist_active();
  return ESP_OK;
}

int hermes_wifi_active_index(void) { return s_active; }
bool hermes_wifi_is_connected(void) { return s_connected; }

int hermes_wifi_rssi(void) {
  if (!s_connected) return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
  return ap.rssi;
}

const hermes_wifi_net_t *hermes_wifi_net(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT) return NULL;
  return &k_nets[index];
}

int hermes_wifi_count(void) { return HERMES_WIFI_COUNT; }
