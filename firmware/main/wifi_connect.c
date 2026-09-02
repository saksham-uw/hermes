#include "wifi_connect.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "hermes_wifi";

static const hermes_wifi_net_t k_nets[HERMES_WIFI_COUNT] = {
    {.ssid = "roger7459", .password = "Sharry22#%", .label = "home wifi"},
    {.ssid = "Saksham",
     .password = "yomamafatvery",
     .label = "personal hotspot"},
    {.ssid = "Adelaide_Club", .password = "4163679957", .label = "office wifi"},
};

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_lock;
static const int WIFI_OK = BIT0;
static const int WIFI_FAIL = BIT1;
static int s_retry = 0;
static int s_active = 0;
static bool s_started;
static bool s_start_attempted;
static esp_err_t s_start_error = ESP_ERR_INVALID_STATE;
static bool s_connected;
static bool s_auto_reconnect = true;
static hermes_wifi_sight_t s_sight[HERMES_WIFI_COUNT];

static esp_err_t apply_sta_config(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT)
    return ESP_ERR_INVALID_ARG;
  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, k_nets[index].ssid,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, k_nets[index].password,
          sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK)
    return err;
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  s_active = index;
  if (s_lock)
    xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "using %s (%s)", k_nets[index].ssid, k_nets[index].label);
  return ESP_OK;
}

static void persist_active(void) {
  nvs_handle_t h;
  if (nvs_open("hermes", NVS_READWRITE, &h) != ESP_OK)
    return;
  nvs_set_i32(h, "wifi_idx", hermes_wifi_active_index());
  nvs_commit(h);
  nvs_close(h);
}

static int load_active(void) {
  nvs_handle_t h;
  int32_t v = 0;
  if (nvs_open("hermes", NVS_READONLY, &h) != ESP_OK)
    return 0;
  nvs_get_i32(h, "wifi_idx", &v);
  nvs_close(h);
  if (v < 0 || v >= HERMES_WIFI_COUNT)
    v = 0;
  return (int)v;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    bool reconnect;
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    reconnect = s_auto_reconnect;
    if (s_lock)
      xSemaphoreGive(s_lock);
    if (reconnect)
      esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    bool reconnect;
    int retry;
    int active;
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connected = false;
    reconnect = s_auto_reconnect && s_retry < 10;
    if (reconnect)
      s_retry++;
    retry = s_retry;
    active = s_active;
    if (s_lock)
      xSemaphoreGive(s_lock);
    if (reconnect) {
      esp_wifi_connect();
      ESP_LOGW(TAG, "retry %d on %s", retry, k_nets[active].ssid);
    } else {
      xEventGroupSetBits(s_wifi_events, WIFI_FAIL);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    int active;
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    s_retry = 0;
    s_connected = true;
    active = s_active;
    if (s_lock)
      xSemaphoreGive(s_lock);
    xEventGroupSetBits(s_wifi_events, WIFI_OK);
    ESP_LOGI(TAG, "connected %s", k_nets[active].ssid);
  }
}

bool hermes_wifi_has_internet(void) {
  if (!hermes_wifi_is_connected())
    return false;
  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0)
    return false;
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
  if (s_started)
    return ESP_OK;
  if (s_start_attempted)
    return s_start_error;
  s_start_attempted = true;

  s_lock = xSemaphoreCreateMutex();
  s_wifi_events = xEventGroupCreate();
  if (!s_lock || !s_wifi_events) {
    s_start_error = ESP_ERR_NO_MEM;
    return s_start_error;
  }
  s_active = load_active();
  memset(s_sight, 0, sizeof(s_sight));

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) {
    s_start_error = err;
    return err;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    s_start_error = err;
    return err;
  }
  if (!esp_netif_create_default_wifi_sta()) {
    s_start_error = ESP_ERR_NO_MEM;
    return s_start_error;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err == ESP_OK)
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi,
                                     NULL);
  if (err == ESP_OK)
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi,
                                     NULL);
  if (err == ESP_OK)
    err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err == ESP_OK)
    err = apply_sta_config(s_active);
  if (err == ESP_OK)
    err = esp_wifi_start();
  if (err != ESP_OK) {
    s_start_error = err;
    ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(err));
    return err;
  }
  s_started = true;
  s_start_error = ESP_OK;
  return ESP_OK;
}

esp_err_t hermes_wifi_connect(void) {
  esp_err_t start = hermes_wifi_start();
  if (start != ESP_OK)
    return start;
  xEventGroupClearBits(s_wifi_events, WIFI_OK | WIFI_FAIL);
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  s_retry = 0;
  s_auto_reconnect = true;
  if (s_lock)
    xSemaphoreGive(s_lock);
  esp_wifi_connect();
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_events, WIFI_OK | WIFI_FAIL, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(25000));
  return (bits & WIFI_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t hermes_wifi_refresh_scan(void) {
  if (!s_started) {
    esp_err_t start = hermes_wifi_start();
    if (start != ESP_OK)
      return start;
  }
  hermes_wifi_sight_t discovered[HERMES_WIFI_COUNT] = {0};

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
  err = esp_wifi_scan_get_ap_num(&ap_num);
  if (err != ESP_OK)
    return err;
  if (ap_num > 40)
    ap_num = 40;

  wifi_ap_record_t *records = NULL;
  uint16_t got = ap_num;
  if (got) {
    records = calloc(got, sizeof(*records));
    if (!records) {
      esp_wifi_clear_ap_list();
      return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err != ESP_OK) {
      free(records);
      return err;
    }
  }

  for (uint16_t i = 0; i < got; i++) {
    for (int n = 0; n < HERMES_WIFI_COUNT; n++) {
      if (strcmp((const char *)records[i].ssid, k_nets[n].ssid) != 0)
        continue;
      if (!discovered[n].visible || records[i].rssi > discovered[n].rssi) {
        discovered[n].visible = true;
        discovered[n].rssi = records[i].rssi;
        discovered[n].channel = records[i].primary;
      }
    }
  }
  free(records);

  /* Active connection always counts as visible. */
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  bool connected = s_connected;
  int active = s_active;
  if (s_lock)
    xSemaphoreGive(s_lock);
  if (connected) {
    discovered[active].visible = true;
    int r = hermes_wifi_rssi();
    if (r != 0)
      discovered[active].rssi = (int8_t)r;
  }
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  memcpy(s_sight, discovered, sizeof(s_sight));
  if (s_lock)
    xSemaphoreGive(s_lock);
  return ESP_OK;
}

bool hermes_wifi_is_selectable(int index) {
  hermes_wifi_sight_t sight;
  return hermes_wifi_sight(index, &sight) && sight.visible;
}

bool hermes_wifi_sight(int index, hermes_wifi_sight_t *out) {
  if (index < 0 || index >= HERMES_WIFI_COUNT || !out)
    return false;
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_sight[index];
  if (s_lock)
    xSemaphoreGive(s_lock);
  return true;
}

static esp_err_t connect_wait(int timeout_ms) {
  xEventGroupClearBits(s_wifi_events, WIFI_OK | WIFI_FAIL);
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  s_retry = 0;
  s_auto_reconnect = true;
  if (s_lock)
    xSemaphoreGive(s_lock);
  esp_wifi_connect();
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_events, WIFI_OK | WIFI_FAIL, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t hermes_wifi_select(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT)
    return ESP_ERR_INVALID_ARG;
  if (!s_started) {
    esp_err_t start = hermes_wifi_start();
    if (start != ESP_OK)
      return start;
  }

  esp_err_t scan = hermes_wifi_refresh_scan();
  if (scan != ESP_OK)
    return scan;
  if (!hermes_wifi_is_selectable(index)) {
    ESP_LOGW(TAG, "%s not in range — keeping current", k_nets[index].ssid);
    return ESP_ERR_NOT_FOUND;
  }
  if (index == hermes_wifi_active_index() && hermes_wifi_is_connected() &&
      hermes_wifi_has_internet()) {
    return ESP_OK;
  }

  int prev = hermes_wifi_active_index();
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  s_connected = false;
  s_auto_reconnect = false;
  if (s_lock)
    xSemaphoreGive(s_lock);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_err_t config = apply_sta_config(index);
  if (config != ESP_OK)
    return config;

  esp_err_t err = connect_wait(18000);
  bool ok = (err == ESP_OK) && hermes_wifi_has_internet();
  if (!ok) {
    ESP_LOGW(TAG, "%s has no internet — reverting to %s", k_nets[index].ssid,
             k_nets[prev].ssid);
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    s_connected = false;
    s_auto_reconnect = false;
    if (s_lock)
      xSemaphoreGive(s_lock);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t restore = apply_sta_config(prev);
    if (restore != ESP_OK)
      return restore;
    connect_wait(18000);
    return ESP_FAIL;
  }
  persist_active();
  return ESP_OK;
}

int hermes_wifi_active_index(void) {
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  int active = s_active;
  if (s_lock)
    xSemaphoreGive(s_lock);
  return active;
}

bool hermes_wifi_is_connected(void) {
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  bool connected = s_connected;
  if (s_lock)
    xSemaphoreGive(s_lock);
  return connected;
}

int hermes_wifi_rssi(void) {
  if (!hermes_wifi_is_connected())
    return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    return 0;
  return ap.rssi;
}

const hermes_wifi_net_t *hermes_wifi_net(int index) {
  if (index < 0 || index >= HERMES_WIFI_COUNT)
    return NULL;
  return &k_nets[index];
}

int hermes_wifi_count(void) { return HERMES_WIFI_COUNT; }
