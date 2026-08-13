#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t hermes_touch_init(void);
bool hermes_touch_read(bool *pressed, uint16_t *x, uint16_t *y);
