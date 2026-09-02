#include "home_data.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "hermes_actions_config.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "home_data";
#define RESPONSE_CAP 4096

typedef struct {
  char data[RESPONSE_CAP];
  size_t length;
  bool overflow;
} response_t;

static void copy_text(char *destination, size_t capacity, const char *source) {
  if (!destination || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source ? source : "");
}

static esp_err_t http_event(esp_http_client_event_t *event) {
  response_t *response = event ? event->user_data : NULL;
  if (!response || event->event_id != HTTP_EVENT_ON_DATA || !event->data)
    return ESP_OK;
  if (response->length + (size_t)event->data_len >= sizeof(response->data)) {
    response->overflow = true;
    return ESP_FAIL;
  }
  memcpy(response->data + response->length, event->data,
         (size_t)event->data_len);
  response->length += (size_t)event->data_len;
  response->data[response->length] = 0;
  return ESP_OK;
}

static esp_err_t fetch_json(const char *path, response_t *response, char *error,
                            size_t error_len) {
  if (!HERMES_ACTIONS_URL[0] || !HERMES_ACTIONS_DEVICE_TOKEN[0]) {
    copy_text(error, error_len, "SERVICE NOT CONFIGURED");
    return ESP_ERR_INVALID_STATE;
  }
  char url[256];
  snprintf(url, sizeof(url), "%s%s", HERMES_ACTIONS_URL, path);
  memset(response, 0, sizeof(*response));
  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 15000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 2048,
      .event_handler = http_event,
      .user_data = response,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    copy_text(error, error_len, "SERVICE OFFLINE");
    return ESP_FAIL;
  }
  char authorization[160];
  snprintf(authorization, sizeof(authorization), "Bearer %s",
           HERMES_ACTIONS_DEVICE_TOKEN);
  esp_http_client_set_header(client, "Authorization", authorization);
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_err_t result = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || response->overflow || status < 200 || status >= 300) {
    ESP_LOGW(TAG, "%s failed: transport=%s status=%d overflow=%d", path,
             esp_err_to_name(result), status, response->overflow);
    copy_text(error, error_len,
              response->overflow ? "RESPONSE TOO LARGE" : "DATA UNAVAILABLE");
    return ESP_FAIL;
  }
  return ESP_OK;
}

static bool number(cJSON *object, const char *key, double *out) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item))
    return false;
  *out = item->valuedouble;
  return true;
}

static const char *string(cJSON *object, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void save_blob(const char *key, const void *value, size_t size) {
  nvs_handle_t handle;
  if (nvs_open("home", NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_set_blob(handle, key, value, size);
  nvs_commit(handle);
  nvs_close(handle);
}

static void load_blob(const char *key, void *value, size_t size) {
  nvs_handle_t handle;
  if (nvs_open("home", NVS_READONLY, &handle) != ESP_OK)
    return;
  size_t found_size = size;
  if (nvs_get_blob(handle, key, value, &found_size) != ESP_OK ||
      found_size != size)
    memset(value, 0, size);
  nvs_close(handle);
}

void home_data_load_cache(home_weather_t *weather, home_markets_t *crypto,
                          home_markets_t *stocks) {
  if (weather)
    load_blob("weather", weather, sizeof(*weather));
  if (crypto)
    load_blob("crypto", crypto, sizeof(*crypto));
  if (stocks)
    load_blob("stocks", stocks, sizeof(*stocks));
}

esp_err_t home_data_fetch_weather(home_weather_t *out, char *error,
                                  size_t error_len) {
  response_t response;
  esp_err_t result =
      fetch_json("/v1/home/weather", &response, error, error_len);
  if (result != ESP_OK)
    return result;
  cJSON *root = cJSON_Parse(response.data);
  double temperature, high, low, rain, observed;
  const char *location = root ? string(root, "location") : NULL;
  const char *condition = root ? string(root, "condition") : NULL;
  bool valid = root && location && condition &&
               number(root, "temperatureC", &temperature) &&
               number(root, "highC", &high) && number(root, "lowC", &low) &&
               number(root, "precipitationPercent", &rain) &&
               number(root, "observedAt", &observed);
  if (!valid) {
    cJSON_Delete(root);
    copy_text(error, error_len, "INVALID WEATHER DATA");
    return ESP_ERR_INVALID_RESPONSE;
  }
  memset(out, 0, sizeof(*out));
  out->valid = true;
  copy_text(out->location, sizeof(out->location), location);
  copy_text(out->condition, sizeof(out->condition), condition);
  out->temperature_c = (float)temperature;
  out->high_c = (float)high;
  out->low_c = (float)low;
  out->precipitation_percent = rain < 0 ? 0 : rain > 100 ? 100 : (uint8_t)rain;
  out->observed_at = (int64_t)observed;
  cJSON_Delete(root);
  save_blob("weather", out, sizeof(*out));
  return ESP_OK;
}

static esp_err_t fetch_markets(const char *path, const char *cache_key,
                               home_markets_t *out, char *error,
                               size_t error_len) {
  response_t response;
  esp_err_t result = fetch_json(path, &response, error, error_len);
  if (result != ESP_OK)
    return result;
  cJSON *root = cJSON_Parse(response.data);
  cJSON *quotes = root ? cJSON_GetObjectItemCaseSensitive(root, "quotes") : NULL;
  const char *status = root ? string(root, "status") : NULL;
  double observed = 0;
  if (!root || !cJSON_IsArray(quotes) || !status ||
      !number(root, "observedAt", &observed)) {
    cJSON_Delete(root);
    copy_text(error, error_len, "INVALID MARKET DATA");
    return ESP_ERR_INVALID_RESPONSE;
  }
  home_markets_t parsed = {.valid = true, .observed_at = (int64_t)observed};
  copy_text(parsed.status, sizeof(parsed.status), status);
  int count = cJSON_GetArraySize(quotes);
  if (count > HOME_MARKET_MAX_QUOTES)
    count = HOME_MARKET_MAX_QUOTES;
  for (int index = 0; index < count; index++) {
    cJSON *quote = cJSON_GetArrayItem(quotes, index);
    double price, change;
    const char *symbol = quote ? string(quote, "symbol") : NULL;
    if (!symbol || !number(quote, "priceUsd", &price) ||
        !number(quote, "changePercent", &change)) {
      cJSON_Delete(root);
      copy_text(error, error_len, "INVALID MARKET QUOTE");
      return ESP_ERR_INVALID_RESPONSE;
    }
    copy_text(parsed.quotes[index].symbol,
              sizeof(parsed.quotes[index].symbol), symbol);
    parsed.quotes[index].price_usd = (float)price;
    parsed.quotes[index].change_percent = (float)change;
    parsed.count++;
  }
  cJSON_Delete(root);
  *out = parsed;
  save_blob(cache_key, out, sizeof(*out));
  return ESP_OK;
}

esp_err_t home_data_fetch_crypto(home_markets_t *out, char *error,
                                 size_t error_len) {
  return fetch_markets("/v1/home/markets/crypto", "crypto", out, error,
                       error_len);
}

esp_err_t home_data_fetch_stocks(home_markets_t *out, char *error,
                                 size_t error_len) {
  return fetch_markets("/v1/home/markets/stocks", "stocks", out, error,
                       error_len);
}
