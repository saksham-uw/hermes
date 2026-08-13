#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define HERMES_WIFI_COUNT 3

typedef struct {
  const char *ssid;
  const char *password;
  const char *label;
} hermes_wifi_net_t;

typedef struct {
  bool visible;
  int8_t rssi;     /* 0 if unknown/not visible */
  uint8_t channel;
} hermes_wifi_sight_t;

esp_err_t hermes_wifi_start(void);
esp_err_t hermes_wifi_connect(void);
/** Switch only if AP is visible; verifies internet; reverts on failure. */
esp_err_t hermes_wifi_select(int index);
int hermes_wifi_active_index(void);
bool hermes_wifi_is_connected(void);
bool hermes_wifi_has_internet(void);
int hermes_wifi_rssi(void);
const hermes_wifi_net_t *hermes_wifi_net(int index);
int hermes_wifi_count(void);

/** Scan APs and fill sight info for baked networks. */
esp_err_t hermes_wifi_refresh_scan(void);
const hermes_wifi_sight_t *hermes_wifi_sight(int index);
bool hermes_wifi_is_selectable(int index);
