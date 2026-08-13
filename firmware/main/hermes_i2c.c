#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_pins.h"
#include "hermes_i2c.h"

static const char *TAG = "hermes_i2c";
static i2c_master_bus_handle_t s_sys;
static i2c_master_bus_handle_t s_touch;

esp_err_t hermes_i2c_init(void) {
  i2c_master_bus_config_t sys = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = HERMES_I2C_SDA,
      .scl_io_num = HERMES_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&sys, &s_sys));

  i2c_master_bus_config_t touch = {
      .i2c_port = I2C_NUM_1,
      .sda_io_num = HERMES_TOUCH_SDA,
      .scl_io_num = HERMES_TOUCH_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&touch, &s_touch));
  ESP_LOGI(TAG, "sys+touch I2C ready");
  return ESP_OK;
}

i2c_master_bus_handle_t hermes_i2c_sys_bus(void) { return s_sys; }
i2c_master_bus_handle_t hermes_i2c_touch_bus(void) { return s_touch; }

esp_err_t hermes_i2c_add_dev(i2c_master_bus_handle_t bus, uint8_t addr_7bit, uint32_t hz,
                             i2c_master_dev_handle_t *out) {
  i2c_device_config_t cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = addr_7bit,
      .scl_speed_hz = hz,
  };
  return i2c_master_bus_add_device(bus, &cfg, out);
}
