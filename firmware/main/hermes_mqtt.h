#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*hermes_down_cb_t)(const char *json, int len);

esp_err_t hermes_mqtt_start(hermes_down_cb_t on_down);
esp_err_t hermes_mqtt_publish_up(const char *json);
bool hermes_mqtt_is_connected(void);
