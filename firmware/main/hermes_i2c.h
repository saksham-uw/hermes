#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t hermes_i2c_init(void);
i2c_master_bus_handle_t hermes_i2c_sys_bus(void);
i2c_master_bus_handle_t hermes_i2c_touch_bus(void);
esp_err_t hermes_i2c_add_dev(i2c_master_bus_handle_t bus, uint8_t addr_7bit, uint32_t hz,
                             i2c_master_dev_handle_t *out);
