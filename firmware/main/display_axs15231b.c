#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_pins.h"
#include "hermes_i2c.h"
#include "display_axs15231b.h"

static const char *TAG = "axs15231b";

static spi_device_handle_t s_spi;
static i2c_master_dev_handle_t s_tca;
static bool s_ready;
static int s_bl_pin = -1;
static int s_bl_pin_b = -1;
static uint8_t s_brightness = 85;

typedef struct {
  uint8_t cmd;
  uint8_t data[4];
  uint8_t len;
  uint16_t delay_ms;
} lcd_cmd_t;

static const lcd_cmd_t k_init[] = {
    {0x11, {0}, 0, 100},
    {0x36, {0x00}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x11, {0}, 0, 100},
    {0x29, {0}, 0, 100},
};

static esp_err_t tca_read(uint8_t reg, uint8_t *val) {
  if (!s_tca || !val) return ESP_ERR_INVALID_STATE;
  return i2c_master_transmit_receive(s_tca, &reg, 1, val, 1, 100);
}

static esp_err_t tca_write(uint8_t reg, uint8_t val) {
  if (!s_tca) return ESP_ERR_INVALID_STATE;
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(s_tca, buf, 2, 100);
}

static esp_err_t tca_set_pin(uint8_t pin, bool high) {
  uint8_t out = 0xFF;
  uint8_t cfg = 0xFF;
  if (tca_read(0x01, &out) != ESP_OK) out = 0xFF;
  if (tca_read(0x03, &cfg) != ESP_OK) cfg = 0xFF;
  if (high) out = (uint8_t)(out | (1u << pin));
  else out = (uint8_t)(out & ~(1u << pin));
  cfg = (uint8_t)(cfg & ~(1u << pin));
  esp_err_t err = tca_write(0x01, out);
  if (err != ESP_OK) return err;
  return tca_write(0x03, cfg);
}

static esp_err_t tca_set_pin_high(uint8_t pin) { return tca_set_pin(pin, true); }

esp_err_t hermes_power_hold_enable(void) {
  if (!s_tca) {
    esp_err_t err = hermes_i2c_add_dev(hermes_i2c_sys_bus(), HERMES_TCA_ADDR, 100000, &s_tca);
    if (err != ESP_OK) return err;
  }
  esp_err_t err = tca_set_pin(HERMES_TCA_SYS_PIN, true);
  if (err == ESP_OK) ESP_LOGI(TAG, "TCA9554 SYS power-hold ON (pin %d)", HERMES_TCA_SYS_PIN);
  else ESP_LOGW(TAG, "SYS power-hold failed: %s", esp_err_to_name(err));
  return err;
}

esp_err_t hermes_power_hold_release(void) {
  if (!s_tca) {
    esp_err_t err = hermes_i2c_add_dev(hermes_i2c_sys_bus(), HERMES_TCA_ADDR, 100000, &s_tca);
    if (err != ESP_OK) return err;
  }
  esp_err_t err = tca_set_pin(HERMES_TCA_SYS_PIN, false);
  if (err == ESP_OK) ESP_LOGI(TAG, "TCA9554 SYS power-hold OFF");
  return err;
}

static void tca_enable_backlight_gate(void) {
  if (!s_tca) {
    if (hermes_i2c_add_dev(hermes_i2c_sys_bus(), HERMES_TCA_ADDR, 100000, &s_tca) != ESP_OK) {
      ESP_LOGW(TAG, "TCA9554 missing");
      return;
    }
  }
  if (tca_set_pin_high(HERMES_TCA_BL_PIN) == ESP_OK)
    ESP_LOGI(TAG, "TCA9554 backlight enable OK");
}

static void send_cmd(uint8_t command, const uint8_t *data, uint32_t length) {
  spi_transaction_t t = {0};
  t.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.cmd = 0x02;
  t.addr = ((uint32_t)command) << 8;
  if (length) {
    t.tx_buffer = data;
    t.length = length * 8;
  }
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void set_col_window(uint16_t x1, uint16_t x2) {
  uint8_t data[] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  send_cmd(0x2A, data, sizeof(data));
}

static void bl_pwm_init(int pin_a, int pin_b) {
  s_bl_pin = pin_a;
  s_bl_pin_b = pin_b;
  ledc_timer_config_t timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_8_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 25000,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&timer);

  ledc_channel_config_t ch0 = {
      .gpio_num = pin_a,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = LEDC_TIMER_0,
      .duty = 255,
      .hpoint = 0,
  };
  ledc_channel_config(&ch0);

  if (pin_b >= 0 && pin_b != pin_a) {
    ledc_channel_config_t ch1 = {
        .gpio_num = pin_b,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ledc_channel_config(&ch1);
  }
}

void hermes_display_set_brightness(uint8_t percent) {
  if (percent < 5) percent = 5;
  if (percent > 100) percent = 100;
  s_brightness = percent;
  /* Waveshare AP3032: active-low PWM — lower duty = brighter.
   * Use almost the full 0..255 range so +/-10% is obvious. */
  uint32_t active = ((uint32_t)percent * 255U) / 100U;
  uint32_t duty = 255U - active;
  if (s_bl_pin >= 0) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  }
  if (s_bl_pin_b >= 0) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
  }
  ESP_LOGI(TAG, "brightness %u%% duty=%lu (active-low)", (unsigned)percent, (unsigned long)duty);
}

void hermes_display_set_backlight(bool on) {
  if (!on) {
    if (s_bl_pin >= 0) {
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (s_bl_pin_b >= 0) {
      ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 255);
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    }
    return;
  }
  hermes_display_set_brightness(s_brightness);
}

esp_err_t hermes_display_init(void) {
  hermes_display_set_backlight(false);

  /* Hold battery rail ASAP so unplugging USB does not brown out. */
  hermes_power_hold_enable();

  gpio_config_t io = {
      .pin_bit_mask = 1ULL << HERMES_LCD_RST,
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io);
  gpio_set_level(HERMES_LCD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(30));
  gpio_set_level(HERMES_LCD_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(250));
  gpio_set_level(HERMES_LCD_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(30));

  spi_bus_config_t bus = {
      .data0_io_num = HERMES_LCD_D0,
      .data1_io_num = HERMES_LCD_D1,
      .sclk_io_num = HERMES_LCD_SCLK,
      .data2_io_num = HERMES_LCD_D2,
      .data3_io_num = HERMES_LCD_D3,
      .max_transfer_sz = 16 * 1024 + 8,
      .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
  };
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  spi_device_interface_config_t dev = {
      .command_bits = 8,
      .address_bits = 24,
      .mode = 3,
      .clock_speed_hz = 40000000,
      .spics_io_num = HERMES_LCD_CS,
      .flags = SPI_DEVICE_HALFDUPLEX,
      .queue_size = 10,
  };
  ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_spi));

  for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
    send_cmd(k_init[i].cmd, k_init[i].data, k_init[i].len);
    if (k_init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(k_init[i].delay_ms));
  }

  tca_enable_backlight_gate();
  hermes_power_hold_enable();
  /* Drive both rev1 (GPIO8) and rev2 (GPIO42) — only the wired one matters. */
  bl_pwm_init(HERMES_LCD_BL_REV1, HERMES_LCD_BL_REV2);
  hermes_display_set_backlight(true);

  s_ready = true;
  ESP_LOGI(TAG, "panel ready native %dx%d (UI %dx%d landscape)", HERMES_PANEL_W, HERMES_PANEL_H,
           HERMES_UI_W, HERMES_UI_H);
  return ESP_OK;
}

bool hermes_display_ready(void) { return s_ready; }

void hermes_display_flush_native(const uint16_t *rgb565_swapped, uint16_t y, uint16_t h) {
  if (!s_ready || !rgb565_swapped || !h) return;

  bool first = true;
  size_t left = (size_t)HERMES_PANEL_W * h;
  const uint16_t *cursor = rgb565_swapped;
  const size_t max_chunk = (16 * 1024) / sizeof(uint16_t);

  set_col_window(0, HERMES_PANEL_W - 1);

  while (left) {
    size_t chunk = left > max_chunk ? max_chunk : left;
    spi_transaction_ext_t t = {0};
    if (first) {
      t.base.flags = SPI_TRANS_MODE_QIO;
      t.base.cmd = 0x32;
      t.base.addr = (y == 0) ? 0x002C00 : 0x003C00;
      first = false;
    } else {
      t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
      t.command_bits = 0;
      t.address_bits = 0;
      t.dummy_bits = 0;
    }
    t.base.tx_buffer = cursor;
    t.base.length = chunk * 16;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
    left -= chunk;
    cursor += chunk;
  }
}
