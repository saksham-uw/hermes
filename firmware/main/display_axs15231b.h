#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t hermes_display_init(void);
void hermes_display_set_backlight(bool on);
void hermes_display_set_brightness(uint8_t percent); /* 1..100 */
/* Push native-order RGB565 already byte-swapped for the panel. Buffer must be
 * DMA-capable (internal RAM) for the duration of the call. */
void hermes_display_flush_native(const uint16_t *rgb565_swapped, uint16_t y, uint16_t h);
bool hermes_display_ready(void);
esp_err_t hermes_power_hold_enable(void);
esp_err_t hermes_power_hold_release(void);
