#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "hermes_gfx.h"

esp_err_t hermes_settings_load(hermes_theme_t *theme, uint8_t *brightness);
esp_err_t hermes_settings_save(hermes_theme_t theme, uint8_t brightness);
