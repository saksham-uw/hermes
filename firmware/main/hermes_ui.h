#pragma once

#include "esp_err.h"

esp_err_t hermes_ui_start(void);
void hermes_ui_on_down_json(const char *json, int len);
