#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "certs_embed.h"
#include "hermes_mqtt.h"

static const char *TAG = "hermes_mqtt";
static esp_mqtt_client_handle_t s_client;
static hermes_down_cb_t s_on_down;
static bool s_connected;

/* Reassemble fragmented MQTT payloads (chats/chat_lines can exceed one chunk). */
static char *s_rx;
static int s_rx_len;
static int s_rx_cap;
static int s_rx_msg_id = -1;

static void topic_up(char *out, size_t n) {
  snprintf(out, n, "hermes/%s/device/up", CONFIG_HERMES_USER_ID);
}

static void topic_down(char *out, size_t n) {
  snprintf(out, n, "hermes/%s/device/down", CONFIG_HERMES_USER_ID);
}

static void rx_reset(void) {
  free(s_rx);
  s_rx = NULL;
  s_rx_len = 0;
  s_rx_cap = 0;
  s_rx_msg_id = -1;
}

static void mqtt_event(void *args, esp_event_base_t base, int32_t id, void *event_data) {
  (void)args;
  (void)base;
  esp_mqtt_event_handle_t ev = event_data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      s_connected = true;
      char down[96];
      topic_down(down, sizeof(down));
      esp_mqtt_client_subscribe(s_client, down, 1);
      ESP_LOGI(TAG, "connected; subscribed %s", down);
      char hello[160];
      snprintf(hello, sizeof(hello),
               "{\"type\":\"hello\",\"deviceId\":\"%s\",\"firmware\":\"0.1.0\"}",
               CONFIG_HERMES_THING_NAME);
      hermes_mqtt_publish_up(hello);
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      s_connected = false;
      rx_reset();
      ESP_LOGW(TAG, "disconnected");
      break;
    case MQTT_EVENT_DATA: {
      if (!s_on_down || ev->data_len < 0) break;

      /* Single-chunk fast path. */
      if (ev->total_data_len == ev->data_len && ev->current_data_offset == 0) {
        s_on_down(ev->data, ev->data_len);
        break;
      }

      if (s_rx_msg_id != ev->msg_id || ev->current_data_offset == 0) {
        rx_reset();
        s_rx_msg_id = ev->msg_id;
        s_rx_cap = ev->total_data_len + 1;
        if (s_rx_cap < 64) s_rx_cap = 64;
        if (s_rx_cap > 48 * 1024) {
          ESP_LOGW(TAG, "down payload too large (%d)", ev->total_data_len);
          break;
        }
        s_rx = malloc((size_t)s_rx_cap);
        if (!s_rx) {
          ESP_LOGW(TAG, "no mem for %d byte payload", ev->total_data_len);
          break;
        }
        s_rx_len = 0;
      }

      if (!s_rx) break;
      if (ev->current_data_offset + ev->data_len > s_rx_cap - 1) {
        ESP_LOGW(TAG, "rx overflow");
        rx_reset();
        break;
      }
      memcpy(s_rx + ev->current_data_offset, ev->data, (size_t)ev->data_len);
      int end = ev->current_data_offset + ev->data_len;
      if (end > s_rx_len) s_rx_len = end;

      if (s_rx_len >= ev->total_data_len) {
        s_rx[s_rx_len] = 0;
        ESP_LOGI(TAG, "down reassembled %d bytes", s_rx_len);
        s_on_down(s_rx, s_rx_len);
        rx_reset();
      }
      break;
    }
    default:
      break;
  }
}

esp_err_t hermes_mqtt_start(hermes_down_cb_t on_down) {
  s_on_down = on_down;
  char uri[160];
  snprintf(uri, sizeof(uri), "mqtts://%s:8883", CONFIG_HERMES_IOT_ENDPOINT);

  const size_t ca_len = hermes_pem_len(hermes_root_ca_start, hermes_root_ca_end);
  const size_t cert_len = hermes_pem_len(hermes_device_cert_start, hermes_device_cert_end);
  const size_t key_len = hermes_pem_len(hermes_device_key_start, hermes_device_key_end);

  esp_mqtt_client_config_t cfg = {
      .broker.address.uri = uri,
      .broker.verification.certificate = hermes_root_ca_start,
      .broker.verification.certificate_len = ca_len,
      .credentials.client_id = CONFIG_HERMES_THING_NAME,
      .credentials.authentication.certificate = hermes_device_cert_start,
      .credentials.authentication.certificate_len = cert_len,
      .credentials.authentication.key = hermes_device_key_start,
      .credentials.authentication.key_len = key_len,
      .session.keepalive = 30,
      .buffer.size = 8192,
      .buffer.out_size = 8192,
  };

  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client) return ESP_FAIL;
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
  return esp_mqtt_client_start(s_client);
}

esp_err_t hermes_mqtt_publish_up(const char *json) {
  if (!s_client || !json) return ESP_ERR_INVALID_STATE;
  char up[96];
  topic_up(up, sizeof(up));
  int msg_id = esp_mqtt_client_publish(s_client, up, json, 0, 1, 0);
  return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

bool hermes_mqtt_is_connected(void) { return s_connected; }
