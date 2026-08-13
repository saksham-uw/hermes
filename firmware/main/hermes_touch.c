#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_pins.h"
#include "hermes_i2c.h"
#include "hermes_touch.h"

static const char *TAG = "hermes_touch";
static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static const uint8_t k_cmd[] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};

esp_err_t hermes_touch_init(void) {
  esp_err_t err = hermes_i2c_add_dev(hermes_i2c_touch_bus(), HERMES_TOUCH_ADDR, 300000, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "touch device add failed %s", esp_err_to_name(err));
    return err;
  }
  s_ok = true;
  ESP_LOGI(TAG, "AXS15231B touch ready @0x%02x", HERMES_TOUCH_ADDR);
  return ESP_OK;
}

bool hermes_touch_read(bool *pressed, uint16_t *x, uint16_t *y) {
  if (!s_ok || !pressed) return false;
  uint8_t buf[8] = {0};
  esp_err_t err = i2c_master_transmit_receive(s_dev, k_cmd, sizeof(k_cmd), buf, sizeof(buf), 50);
  if (err != ESP_OK) {
    *pressed = false;
    return false;
  }
  uint8_t points = buf[1];
  if (points == 0 || points > 4) {
    *pressed = false;
    return true;
  }
  uint16_t raw_long = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
  uint16_t raw_short = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
  /* Physical panel coords (native portrait). */
  uint16_t px = raw_short;
  uint16_t py = (raw_long >= HERMES_PANEL_H) ? 0 : (uint16_t)(HERMES_PANEL_H - 1 - raw_long);
  if (px >= HERMES_PANEL_W) px = HERMES_PANEL_W - 1;
  if (py >= HERMES_PANEL_H) py = HERMES_PANEL_H - 1;

  /* Map to RSVP LandscapeFlipped logical coords. */
  uint16_t lx = py;
  uint16_t ly = (uint16_t)(HERMES_UI_H - 1 - px);
  if (lx >= HERMES_UI_W) lx = HERMES_UI_W - 1;
  if (ly >= HERMES_UI_H) ly = HERMES_UI_H - 1;

  *pressed = true;
  if (x) *x = lx;
  if (y) *y = ly;
  return true;
}
