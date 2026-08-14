#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "board_pins.h"
#include "hermes_i2c.h"
#include "display_axs15231b.h"
#include "hermes_mqtt.h"
#include "hermes_audio.h"

static const char *TAG = "hermes_audio";

#define SAMPLE_HZ 16000
#define REC_MAX_MS 6000

static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_rec;
static bool s_ok;
static volatile bool s_recording;
static int16_t *s_pcm;
static int s_pcm_bytes;
static int s_pcm_cap;

static const char k_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *src, int n, char *dst, int dst_cap) {
  int o = 0;
  for (int i = 0; i < n; i += 3) {
    int rem = n - i;
    uint32_t v = ((uint32_t)src[i]) << 16;
    if (rem > 1) v |= ((uint32_t)src[i + 1]) << 8;
    if (rem > 2) v |= src[i + 2];
    if (o + 4 >= dst_cap) return o;
    dst[o++] = k_b64[(v >> 18) & 63];
    dst[o++] = k_b64[(v >> 12) & 63];
    dst[o++] = rem > 1 ? k_b64[(v >> 6) & 63] : '=';
    dst[o++] = rem > 2 ? k_b64[v & 63] : '=';
  }
  dst[o] = 0;
  return o;
}

static void rec_task(void *arg) {
  (void)arg;
  uint8_t *tmp = heap_caps_malloc(1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!tmp) {
    s_recording = false;
    vTaskDelete(NULL);
    return;
  }
  while (s_recording && s_pcm_bytes + 512 <= s_pcm_cap) {
    int err = esp_codec_dev_read(s_rec, tmp, 1024);
    if (err != ESP_CODEC_DEV_OK) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    /* Stereo 16-bit → mono 16-bit (left). */
    int16_t *in = (int16_t *)tmp;
    int frames = 1024 / 4;
    int16_t *out = (int16_t *)((uint8_t *)s_pcm + s_pcm_bytes);
    for (int i = 0; i < frames; i++) out[i] = in[i * 2];
    s_pcm_bytes += frames * 2;
  }
  free(tmp);
  s_recording = false;
  vTaskDelete(NULL);
}

static void upload_task(void *arg) {
  (void)arg;
  int bytes = s_pcm_bytes;
  int16_t *pcm = s_pcm;
  s_pcm = NULL;
  s_pcm_bytes = 0;
  if (!pcm || bytes < 320) {
    free(pcm);
    ESP_LOGW(TAG, "clip too short (%d)", bytes);
    hermes_mqtt_publish_up(
        "{\"type\":\"voice_end\",\"id\":\"\",\"error\":\"too short\"}");
    vTaskDelete(NULL);
    return;
  }

  char id[12];
  snprintf(id, sizeof(id), "%08lx", (unsigned long)(esp_random() & 0xffffffff));
  char begin[96];
  snprintf(begin, sizeof(begin),
           "{\"type\":\"voice_begin\",\"id\":\"%s\",\"hz\":%d,\"bytes\":%d}", id,
           SAMPLE_HZ, bytes);
  hermes_mqtt_publish_up(begin);

  const int raw_chunk = 1800;
  char *b64 = malloc(2408);
  char *msg = malloc(2600);
  if (!b64 || !msg) {
    free(b64);
    free(msg);
    free(pcm);
    vTaskDelete(NULL);
    return;
  }
  int seq = 0;
  for (int off = 0; off < bytes; off += raw_chunk) {
    int n = bytes - off;
    if (n > raw_chunk) n = raw_chunk;
    b64_encode((const uint8_t *)pcm + off, n, b64, 2408);
    snprintf(msg, 2600, "{\"type\":\"voice_chunk\",\"id\":\"%s\",\"seq\":%d,\"d\":\"%s\"}",
             id, seq++, b64);
    hermes_mqtt_publish_up(msg);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  snprintf(msg, 2600, "{\"type\":\"voice_end\",\"id\":\"%s\"}", id);
  hermes_mqtt_publish_up(msg);
  ESP_LOGI(TAG, "uploaded voice id=%s bytes=%d chunks=%d", id, bytes, seq);
  free(b64);
  free(msg);
  free(pcm);
  vTaskDelete(NULL);
}

esp_err_t hermes_audio_init(void) {
  if (hermes_codec_power_enable() != ESP_OK) {
    ESP_LOGW(TAG, "codec power pin failed");
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
  if (err != ESP_OK) return err;

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_HZ),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = HERMES_I2S_MCLK,
              .bclk = HERMES_I2S_BCLK,
              .ws = HERMES_I2S_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = HERMES_I2S_DIN,
              .invert_flags = {0},
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  err = i2s_channel_init_std_mode(s_rx, &std_cfg);
  if (err != ESP_OK) return err;
  i2s_channel_enable(s_rx);

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = I2S_NUM_0,
      .rx_handle = s_rx,
      .tx_handle = NULL,
  };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
  if (!data_if) return ESP_FAIL;

  audio_codec_i2c_cfg_t i2c_cfg = {
      .port = I2C_NUM_0,
      .addr = ES7210_CODEC_DEFAULT_ADDR,
      .bus_handle = hermes_i2c_sys_bus(),
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
  if (!ctrl_if) return ESP_FAIL;

  es7210_codec_cfg_t es_cfg = {
      .ctrl_if = ctrl_if,
      .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
  };
  const audio_codec_if_t *codec_if = es7210_codec_new(&es_cfg);
  if (!codec_if) return ESP_FAIL;

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = codec_if,
      .data_if = data_if,
  };
  s_rec = esp_codec_dev_new(&dev_cfg);
  if (!s_rec) return ESP_FAIL;

  esp_codec_dev_sample_info_t fs = {
      .sample_rate = SAMPLE_HZ,
      .channel = 2,
      .bits_per_sample = 16,
  };
  err = esp_codec_dev_open(s_rec, &fs);
  if (err != ESP_CODEC_DEV_OK) {
    ESP_LOGW(TAG, "es7210 open failed %d", err);
    return ESP_FAIL;
  }
  esp_codec_dev_set_in_gain(s_rec, 30.0);
  s_ok = true;
  ESP_LOGI(TAG, "ES7210 record ready %d Hz", SAMPLE_HZ);
  return ESP_OK;
}

bool hermes_audio_ok(void) { return s_ok; }
bool hermes_audio_recording(void) { return s_recording; }

esp_err_t hermes_audio_start(void) {
  if (!s_ok || s_recording) return ESP_ERR_INVALID_STATE;
  s_pcm_cap = SAMPLE_HZ * 2 * REC_MAX_MS / 1000;
  s_pcm = heap_caps_malloc((size_t)s_pcm_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pcm) s_pcm = malloc((size_t)s_pcm_cap);
  if (!s_pcm) return ESP_ERR_NO_MEM;
  s_pcm_bytes = 0;
  s_recording = true;
  if (xTaskCreate(rec_task, "rec", 4096, NULL, 5, NULL) != pdPASS) {
    s_recording = false;
    free(s_pcm);
    s_pcm = NULL;
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t hermes_audio_stop_and_upload(void) {
  if (!s_recording && !s_pcm) return ESP_ERR_INVALID_STATE;
  s_recording = false;
  vTaskDelay(pdMS_TO_TICKS(40));
  if (xTaskCreate(upload_task, "stt_up", 6144, NULL, 4, NULL) != pdPASS) {
    free(s_pcm);
    s_pcm = NULL;
    s_pcm_bytes = 0;
    return ESP_FAIL;
  }
  return ESP_OK;
}
