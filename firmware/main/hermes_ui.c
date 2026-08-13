#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include "hermes_ui.h"
#include "hermes_mqtt.h"

static const char *TAG = "hermes_ui";

/* Waveshare boards expose BOOT on GPIO 0. Use it as a console UI control when
 * LVGL/touch BSP is not linked yet (CONFIG_HERMES_UI_CONSOLE). */
#ifndef CONFIG_HERMES_BOOT_GPIO
#define CONFIG_HERMES_BOOT_GPIO 0
#endif

typedef enum { SCREEN_STATUS = 0, SCREEN_APPROVAL = 1, SCREEN_PROMPT = 2 } screen_t;

static screen_t s_screen = SCREEN_STATUS;
static char s_status[128] = "boot";
static char s_agent[16] = "codex";
static char s_approval_id[64] = "";
static char s_approval_title[96] = "";
static char s_approval_detail[200] = "";
static int s_prompt_idx = 0;

static const char *PRESETS[] = {
    "status",
    "continue",
    "summarize what you did",
    "stop and wait for me",
};

static void publish_json(cJSON *obj) {
  char *printed = cJSON_PrintUnformatted(obj);
  if (printed) {
    hermes_mqtt_publish_up(printed);
    free(printed);
  }
  cJSON_Delete(obj);
}

static void send_approve(bool approve) {
  if (s_approval_id[0] == '\0') return;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "type", approve ? "approve" : "deny");
  cJSON_AddStringToObject(o, "id", s_approval_id);
  publish_json(o);
  ESP_LOGI(TAG, "%s %s", approve ? "approve" : "deny", s_approval_id);
  s_approval_id[0] = '\0';
  s_screen = SCREEN_STATUS;
}

static void send_prompt(const char *text) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "type", "prompt");
  cJSON_AddStringToObject(o, "agent", "codex");
  cJSON_AddStringToObject(o, "text", text);
  publish_json(o);
  ESP_LOGI(TAG, "prompt: %s", text);
}

static void render(void) {
  printf("\n======== HERMES ========\n");
  printf("mqtt: %s\n", hermes_mqtt_is_connected() ? "up" : "down");
  if (s_screen == SCREEN_STATUS) {
    printf("[STATUS] agent=%s\n%s\n", s_agent, s_status);
    printf("BOOT short: next screen\n");
  } else if (s_screen == SCREEN_APPROVAL) {
    printf("[APPROVAL]\n%s\n%s\nid=%s\n", s_approval_title, s_approval_detail, s_approval_id);
    printf("BOOT short: DENY | BOOT long(>1s): APPROVE\n");
  } else {
    printf("[PROMPT] preset %d: %s\n", s_prompt_idx, PRESETS[s_prompt_idx]);
    printf("BOOT short: next preset | BOOT long: send\n");
  }
  printf("========================\n");
}

void hermes_ui_on_down_json(const char *json, int len) {
  char *buf = calloc(1, (size_t)len + 1);
  if (!buf) return;
  memcpy(buf, json, (size_t)len);
  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) return;

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  if (cJSON_IsString(type)) {
    if (strcmp(type->valuestring, "status") == 0) {
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      const cJSON *summary = cJSON_GetObjectItem(root, "summary");
      const cJSON *state = cJSON_GetObjectItem(root, "state");
      if (cJSON_IsString(agent)) strncpy(s_agent, agent->valuestring, sizeof(s_agent) - 1);
      if (cJSON_IsString(summary)) strncpy(s_status, summary->valuestring, sizeof(s_status) - 1);
      if (cJSON_IsString(state)) {
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s: %s", state->valuestring, s_status);
        strncpy(s_status, tmp, sizeof(s_status) - 1);
      }
    } else if (strcmp(type->valuestring, "log") == 0) {
      const cJSON *text = cJSON_GetObjectItem(root, "text");
      if (cJSON_IsString(text)) strncpy(s_status, text->valuestring, sizeof(s_status) - 1);
    } else if (strcmp(type->valuestring, "approval") == 0) {
      const cJSON *id = cJSON_GetObjectItem(root, "id");
      const cJSON *title = cJSON_GetObjectItem(root, "title");
      const cJSON *detail = cJSON_GetObjectItem(root, "detail");
      if (cJSON_IsString(id)) strncpy(s_approval_id, id->valuestring, sizeof(s_approval_id) - 1);
      if (cJSON_IsString(title)) strncpy(s_approval_title, title->valuestring, sizeof(s_approval_title) - 1);
      if (cJSON_IsString(detail)) strncpy(s_approval_detail, detail->valuestring, sizeof(s_approval_detail) - 1);
      s_screen = SCREEN_APPROVAL;
    }
  }
  cJSON_Delete(root);
  render();
}

static void boot_task(void *arg) {
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << CONFIG_HERMES_BOOT_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io);

  bool prev = true;
  TickType_t down_at = 0;
  while (1) {
    bool level = gpio_get_level(CONFIG_HERMES_BOOT_GPIO); // active low
    if (prev && !level) {
      down_at = xTaskGetTickCount();
    } else if (!prev && level) {
      TickType_t held = xTaskGetTickCount() - down_at;
      bool long_press = held > pdMS_TO_TICKS(1000);
      if (s_screen == SCREEN_STATUS) {
        s_screen = SCREEN_APPROVAL;
        if (s_approval_id[0] == '\0') s_screen = SCREEN_PROMPT;
      } else if (s_screen == SCREEN_APPROVAL) {
        if (long_press) send_approve(true);
        else send_approve(false);
      } else {
        if (long_press) send_prompt(PRESETS[s_prompt_idx]);
        else s_prompt_idx = (s_prompt_idx + 1) % (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
      }
      render();
    }
    prev = level;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

esp_err_t hermes_ui_start(void) {
  ESP_LOGI(TAG, "console UI (LVGL/touch can replace this when Waveshare BSP is linked)");
  render();
  xTaskCreate(boot_task, "hermes_boot", 4096, NULL, 5, NULL);
  return ESP_OK;
}
