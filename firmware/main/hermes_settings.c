#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "hermes_sd.h"
#include "hermes_settings.h"

static const char *TAG = "hermes_cfg";
#define SETTINGS_PATH HERMES_SD_MOUNT_POINT "/hermes.cfg"

static hermes_theme_t theme_from_name(const char *name) {
  if (!name) return THEME_NIGHT;
  if (strcmp(name, "terminal") == 0) return THEME_TERMINAL;
  if (strcmp(name, "abyss") == 0) return THEME_ABYSS;
  if (strcmp(name, "light") == 0) return THEME_LIGHT;
  if (strcmp(name, "sumi") == 0 || strcmp(name, "sumi ink") == 0) return THEME_SUMI;
  if (strcmp(name, "porcelain") == 0) return THEME_PORCELAIN;
  if (strcmp(name, "fog") == 0) return THEME_FOG;
  if (strcmp(name, "night") == 0) return THEME_NIGHT;
  /* numeric fallback */
  char *end = NULL;
  long v = strtol(name, &end, 10);
  if (end != name && v >= 0 && v < THEME_COUNT) return (hermes_theme_t)v;
  return THEME_NIGHT;
}

esp_err_t hermes_settings_load(hermes_theme_t *theme, uint8_t *brightness) {
  if (!hermes_sd_ready()) return ESP_ERR_INVALID_STATE;

  FILE *f = fopen(SETTINGS_PATH, "r");
  if (!f) {
    ESP_LOGI(TAG, "no %s yet", SETTINGS_PATH);
    return ESP_ERR_NOT_FOUND;
  }

  char line[64];
  while (fgets(line, sizeof(line), f)) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const char *key = line;
    const char *val = eq + 1;
    if (strcmp(key, "theme") == 0 && theme) {
      *theme = theme_from_name(val);
    } else if (strcmp(key, "brightness") == 0 && brightness) {
      int b = atoi(val);
      if (b < 5) b = 5;
      if (b > 100) b = 100;
      *brightness = (uint8_t)b;
    }
  }
  fclose(f);
  ESP_LOGI(TAG, "loaded theme=%s brightness=%u", hermes_theme_name(*theme),
           (unsigned)*brightness);
  return ESP_OK;
}

esp_err_t hermes_settings_save(hermes_theme_t theme, uint8_t brightness) {
  if (!hermes_sd_ready()) return ESP_ERR_INVALID_STATE;
  if (brightness < 5) brightness = 5;
  if (brightness > 100) brightness = 100;
  if (theme >= THEME_COUNT) theme = THEME_NIGHT;

  FILE *f = fopen(SETTINGS_PATH, "w");
  if (!f) {
    ESP_LOGW(TAG, "cannot write %s", SETTINGS_PATH);
    return ESP_FAIL;
  }
  fprintf(f, "theme=%s\nbrightness=%u\n", hermes_theme_name(theme), (unsigned)brightness);
  fclose(f);
  ESP_LOGI(TAG, "saved theme=%s brightness=%u", hermes_theme_name(theme), (unsigned)brightness);
  return ESP_OK;
}
