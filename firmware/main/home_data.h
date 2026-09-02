#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define HOME_MARKET_MAX_QUOTES 4

typedef struct {
  bool valid;
  char location[20];
  char condition[24];
  float temperature_c;
  float high_c;
  float low_c;
  uint8_t precipitation_percent;
  int64_t observed_at;
} home_weather_t;

typedef struct {
  char symbol[8];
  float price_usd;
  float change_percent;
} home_quote_t;

typedef struct {
  bool valid;
  uint8_t count;
  char status[10];
  int64_t observed_at;
  home_quote_t quotes[HOME_MARKET_MAX_QUOTES];
} home_markets_t;

void home_data_load_cache(home_weather_t *weather, home_markets_t *crypto,
                          home_markets_t *stocks);
esp_err_t home_data_fetch_weather(home_weather_t *out, char *error,
                                  size_t error_len);
esp_err_t home_data_fetch_crypto(home_markets_t *out, char *error,
                                 size_t error_len);
esp_err_t home_data_fetch_stocks(home_markets_t *out, char *error,
                                 size_t error_len);

