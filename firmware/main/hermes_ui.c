#include "hermes_ui.h"
#include "board_pins.h"
#include "display_axs15231b.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hermes_avatar.h"
#include "hermes_gfx.h"
#include "hermes_i2c.h"
#include "hermes_settings.h"
#include "hermes_touch.h"
#include "hermes_actions.h"
#include "sdkconfig.h"
#include "wifi_connect.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "hermes_ui";

#define PWR_HOLD_MS 900U
#define BTN_DEBOUNCE_MS 35U
#define TOUCH_MIN_SAMPLES 2
#define STATUS_SAMPLES 26
#define SETTINGS_COUNT 2
#define INPUT_QUEUE_LENGTH 16
#define WIFI_WORKER_STACK_BYTES 6144
#define GIT_WORKER_STACK_BYTES 12288
#define GIT_REPO_COUNT 2
#define GIT_MENU_COUNT 2
#define WORKFLOW_COUNT 2
#define GIT_CONFIRM_COUNT 2
#define MENU_TOP 22
#define MENU_ROW_HEIGHT 25

typedef enum {
  SCR_MENU = 0,
  SCR_GIT,
  SCR_GIT_MENU,
  SCR_WORKFLOWS,
  SCR_GIT_LOADING,
  SCR_GIT_BRANCHES,
  SCR_GIT_PULLS,
  SCR_GIT_DETAIL,
  SCR_GIT_CONFIRM,
  SCR_GIT_RESULT,
  SCR_STATUS,
  SCR_SETTINGS,
  SCR_WIFI,
} screen_t;

typedef enum {
  GIT_JOB_BRANCHES = 0,
  GIT_JOB_CREATE_PROD,
  GIT_JOB_CREATE_BRANCH,
  GIT_JOB_PULLS,
  GIT_JOB_MERGE,
} git_job_t;

typedef struct {
  bool previous_level;
  bool long_fired;
  TickType_t down_tick;
} button_t;

typedef struct {
  uint8_t pin;
  bool level;
  TickType_t tick;
} input_edge_t;

static hermes_gfx_t s_gfx;
static hermes_avatar_t s_avatar;
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty = true;
static screen_t s_screen = SCR_MENU;
static int s_menu_index;
static int s_git_repo_index;
static int s_git_index;
static int s_workflow_index;
static int s_git_branch_index;
static int s_git_pull_index;
static int s_git_confirm_index;
static int s_git_branch_count;
static int s_git_pull_count;
static bool s_git_busy;
static git_job_t s_git_job;
static hermes_git_branch_t s_git_branches[HERMES_GIT_MAX_BRANCHES];
static hermes_git_pull_t s_git_pulls[HERMES_GIT_MAX_PULLS];
static hermes_git_pull_t s_git_detail;
static char s_git_job_branch[HERMES_GIT_NAME_LEN];
static char s_git_job_key[40];
static char s_git_loading_title[48];
static char s_git_result_title[48];
static char s_git_result_detail[80];
static screen_t s_git_back = SCR_GIT;
static int s_settings_index;
static int s_wifi_index;
static bool s_wifi_scanning;
static bool s_wifi_busy;
static uint32_t s_navigation_generation;
static char s_wifi_result[56];
static hermes_theme_t s_theme = THEME_NIGHT;
static uint8_t s_brightness = 85;

static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ready;
static int s_battery_percent = -1;
static int s_battery_mv = -1;
static int8_t s_rssi_history[STATUS_SAMPLES];
static uint8_t s_rssi_count;
static uint8_t s_rssi_cursor;
static uint32_t s_last_status_ms;
static QueueHandle_t s_input_queue;
static volatile uint32_t s_input_queue_overflows;

static const char *MENU[] = {"git", "status", "settings", "wifi", "switch off"};
static const int MENU_COUNT = sizeof(MENU) / sizeof(MENU[0]);
static const char *GIT_REPOS[] = {"ultracontract", "ultrachatbot"};
static const char *GIT_MENU[] = {"workflows", "merge"};
static const char *WORKFLOWS[] = {"git [main <- branch]", "git [prod <- main]"};
static const char *GIT_CONFIRM[] = {"confirm merge", "cancel"};

static uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
static void mark_dirty(void) { s_dirty = true; }

static void input_edge_isr(void *argument) {
  input_edge_t edge = {
      .pin = (uint8_t)(uintptr_t)argument,
      .tick = xTaskGetTickCountFromISR(),
  };
  edge.level = gpio_get_level(edge.pin) != 0;
  BaseType_t task_woken = pdFALSE;
  if (xQueueSendFromISR(s_input_queue, &edge, &task_woken) != pdTRUE)
    s_input_queue_overflows++;
  if (task_woken)
    portYIELD_FROM_ISR();
}

static esp_err_t input_init(void) {
  s_input_queue = xQueueCreate(INPUT_QUEUE_LENGTH, sizeof(input_edge_t));
  if (!s_input_queue)
    return ESP_ERR_NO_MEM;

  gpio_config_t input = {
      .pin_bit_mask = (1ULL << HERMES_BOOT_PIN) | (1ULL << HERMES_PWR_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&input);
  if (err != ESP_OK)
    return err;

  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    return err;
  err = gpio_isr_handler_add(HERMES_BOOT_PIN, input_edge_isr,
                             (void *)(uintptr_t)HERMES_BOOT_PIN);
  if (err != ESP_OK)
    return err;
  return gpio_isr_handler_add(HERMES_PWR_PIN, input_edge_isr,
                              (void *)(uintptr_t)HERMES_PWR_PIN);
}

static void note_activity(void) {
  hermes_avatar_note_activity(&s_avatar, now_ms());
}

static void draw_avatar_panel(int cx, int cy) {
  hermes_avatar_draw(&s_gfx, &s_avatar, cx, cy, now_ms());
  char identity[40];
  snprintf(identity, sizeof(identity), "HERMES // %s / %s",
           hermes_avatar_state_name(&s_avatar),
           hermes_avatar_action_name(&s_avatar));
  int identity_x = cx - hermes_gfx_text_width(identity, 1) / 2;
  hermes_gfx_text(&s_gfx, identity_x, cy + 59, identity, COL_DIM, 1);
}

static void adc_init(void) {
  adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
  if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK)
    return;
  adc_oneshot_chan_cfg_t channel = {
      .bitwidth = ADC_BITWIDTH_DEFAULT,
      .atten = ADC_ATTEN_DB_12,
  };
  if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL_3, &channel) == ESP_OK)
    s_adc_ready = true;
}

static void update_status_sample(uint32_t now) {
  if (s_last_status_ms && now - s_last_status_ms < 1000U)
    return;
  s_last_status_ms = now;

  if (s_adc_ready) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_3, &raw) == ESP_OK) {
      int pack_mv = (raw * 3100 / 4095) * 3;
      if (pack_mv >= 2500) {
        s_battery_mv = pack_mv;
        s_battery_percent = (pack_mv - 3300) * 100 / 900;
        if (s_battery_percent < 0)
          s_battery_percent = 0;
        if (s_battery_percent > 100)
          s_battery_percent = 100;
      } else {
        s_battery_mv = -1;
        s_battery_percent = -1;
      }
    }
  }

  int rssi = hermes_wifi_rssi();
  if (rssi > 0 || rssi < -127)
    rssi = -100;
  s_rssi_history[s_rssi_cursor] = (int8_t)rssi;
  s_rssi_cursor = (uint8_t)((s_rssi_cursor + 1U) % STATUS_SAMPLES);
  if (s_rssi_count < STATUS_SAMPLES)
    s_rssi_count++;
  if (s_screen == SCR_STATUS)
    mark_dirty();
}

static void draw_progress(int x, int y, int width, int percent,
                          uint16_t color) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
  hermes_gfx_rect(&s_gfx, x, y, width, 9, COL_LINE);
  int fill = (width - 4) * percent / 100;
  if (fill > 0)
    hermes_gfx_fill_rect(&s_gfx, x + 2, y + 2, fill, 5, color);
}

static void draw_footer(const char *text) {
  hermes_gfx_hline(&s_gfx, 10, GFX_H - 18, GFX_W - 20, COL_LINE);
  hermes_gfx_text(&s_gfx, 12, GFX_H - 13, text, COL_DIM, 1);
}

static void boot_screen(bool shutting_down) {
  static const char *boot_subtitles[] = {
      "finding the horizon",
      "waking the courier",
      "polishing winged boots",
      "giving Hermes a pulse",
  };
  static const char *off_subtitles[] = {
      "folding the map",
      "parking the winged boots",
      "Hermes is going quiet",
      "see you on the next route",
  };
  const char **subtitles = shutting_down ? off_subtitles : boot_subtitles;
  const uint32_t duration_ms = shutting_down ? 1500U : 3000U;
  const int frames = shutting_down ? 15 : 30;
  const uint32_t started_at = now_ms();

  for (int frame = 0; frame <= frames; frame++) {
    if (frame > 0) {
      uint32_t target =
          started_at + duration_ms * (uint32_t)frame / (uint32_t)frames;
      int32_t wait_ms = (int32_t)(target - now_ms());
      if (wait_ms > 0)
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
    }
    hermes_gfx_clear(&s_gfx, COL_BG);
    int below_logo = hermes_gfx_draw_hermes_logo(&s_gfx, 16, 8, COL_FOCUS, 1);
    int bar_y = below_logo + 4;
    int bar_width = GFX_W - 32;
    hermes_gfx_rect(&s_gfx, 16, bar_y, bar_width, 12, COL_LINE);
    int fill = (bar_width - 4) * frame / frames;
    if (fill > 0)
      hermes_gfx_fill_rect(&s_gfx, 18, bar_y + 2, fill, 8, COL_FOCUS);
    int subtitle = (frame * 4) / (frames + 1);
    hermes_gfx_text(&s_gfx, 16, bar_y + 20, subtitles[subtitle], COL_DIM, 1);
    hermes_gfx_flush(&s_gfx);
  }

  if (!shutting_down)
    return;

  /* A held PWR line would immediately wake deep sleep. Finish the animation,
   * then wait for release before dropping the battery power hold. */
  while (gpio_get_level(HERMES_PWR_PIN) == 0)
    vTaskDelay(pdMS_TO_TICKS(10));
  hermes_display_set_backlight(false);
  hermes_power_hold_release();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 0);
  esp_deep_sleep_start();
}

static void draw_menu(void) {
  hermes_gfx_text(&s_gfx, 12, 5, "HERMES // POCKET SYSTEM", COL_DIM, 1);
  hermes_gfx_hline(&s_gfx, 12, 16, GFX_W - 24, COL_LINE);

  for (int i = 0; i < MENU_COUNT; i++) {
    bool selected = i == s_menu_index;
    int row_top = MENU_TOP + i * MENU_ROW_HEIGHT;
    char row[32];
    snprintf(row, sizeof(row), "%s %s", selected ? ">" : " ", MENU[i]);
    if (selected)
      hermes_gfx_rect(&s_gfx, 12, row_top, 378, MENU_ROW_HEIGHT - 2, COL_FOCUS);
    hermes_gfx_text(&s_gfx, 18, row_top + 4, row,
                    selected ? COL_FOCUS : COL_TEXT, 2);
  }

  hermes_gfx_vline(&s_gfx, 403, 24, 118, COL_LINE);
  draw_avatar_panel(516, 83);
  draw_footer("BOOT next   TAP select   PWR back/hold off");
}

static void draw_panel(int x, int width, const char *title) {
  hermes_gfx_rect(&s_gfx, x, 25, width, 125, COL_LINE);
  hermes_gfx_text(&s_gfx, x + 8, 31, title, COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, x + 6, 42, width - 12, COL_LINE);
}

static int signal_percent(int rssi) {
  if (rssi == 0)
    return 0;
  int value = (rssi + 90) * 100 / 60;
  if (value < 0)
    return 0;
  if (value > 100)
    return 100;
  return value;
}

static void draw_status(void) {
  uint32_t uptime = now_ms() / 1000U;
  int rssi = hermes_wifi_rssi();
  int signal = signal_percent(rssi);
  const hermes_wifi_net_t *active = hermes_wifi_net(hermes_wifi_active_index());
  char text[48];

  hermes_gfx_text(&s_gfx, 12, 6, "// HERMES TELEMETRY :: LIVE", COL_FOCUS, 1);
  hermes_gfx_text(&s_gfx, GFX_W - 70, 6, "[SYS:OK]", COL_OK, 1);
  hermes_gfx_hline(&s_gfx, 12, 18, GFX_W - 24, COL_LINE);

  draw_panel(8, 190, "[PWR] POWER CORE");
  snprintf(text, sizeof(text), "BAT  %s",
           s_battery_percent >= 0 ? "ONLINE" : "USB/UNKNOWN");
  hermes_gfx_text(&s_gfx, 16, 50, text,
                  s_battery_percent >= 0 ? COL_OK : COL_WARN, 1);
  if (s_battery_percent >= 0) {
    snprintf(text, sizeof(text), "%d%%  %dmV", s_battery_percent, s_battery_mv);
    hermes_gfx_text(&s_gfx, 16, 63, text, COL_TEXT, 1);
    draw_progress(16, 75, 174, s_battery_percent,
                  s_battery_percent < 20 ? COL_BAD : COL_OK);
  } else {
    hermes_gfx_text(&s_gfx, 16, 63, "--%  rail external", COL_DIM, 1);
    draw_progress(16, 75, 174, 0, COL_DIM);
  }
  snprintf(text, sizeof(text), "UP   %02lu:%02lu:%02lu",
           (unsigned long)(uptime / 3600U),
           (unsigned long)((uptime / 60U) % 60U),
           (unsigned long)(uptime % 60U));
  hermes_gfx_text(&s_gfx, 16, 93, text, COL_TEXT, 1);
  snprintf(text, sizeof(text), "CPU  %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  hermes_gfx_text(&s_gfx, 16, 108, text, COL_TEXT, 1);
  snprintf(text, sizeof(text), "HEAP %luK",
           (unsigned long)(esp_get_free_heap_size() / 1024U));
  hermes_gfx_text(&s_gfx, 16, 123, text, COL_TEXT, 1);
  snprintf(text, sizeof(text), "PSR  %luK",
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024U));
  hermes_gfx_text(&s_gfx, 16, 138, text, COL_DIM, 1);

  draw_panel(205, 195, "[RF] NETWORK LINK");
  snprintf(text, sizeof(text), "AP   %s", active ? active->ssid : "none");
  hermes_gfx_text(&s_gfx, 213, 50, text, COL_TEXT, 1);
  snprintf(text, sizeof(text), "LINK %s",
           hermes_wifi_is_connected() ? "ASSOCIATED" : "OFFLINE");
  hermes_gfx_text(&s_gfx, 213, 65, text,
                  hermes_wifi_is_connected() ? COL_OK : COL_BAD, 1);
  snprintf(text, sizeof(text), "RSSI %ddBm", rssi);
  hermes_gfx_text(&s_gfx, 213, 80, text, rssi ? COL_TEXT : COL_DIM, 1);
  draw_progress(213, 94, 179, signal, COL_FOCUS);
  snprintf(text, sizeof(text), "SIG  %03d/100", signal);
  hermes_gfx_text(&s_gfx, 213, 111, text, COL_TEXT, 1);
  hermes_gfx_text(&s_gfx, 213, 127,
                  hermes_wifi_is_connected() ? "ROUTE :: READY"
                                             : "ROUTE :: DARK",
                  hermes_wifi_is_connected() ? COL_OK : COL_WARN, 1);

  draw_panel(407, 225, "[::] SIGNAL TRACE");
  for (int grid = 0; grid < 4; grid++) {
    int y = 55 + grid * 23;
    hermes_gfx_hline(&s_gfx, 415, y, 209, COL_LINE);
  }
  for (uint8_t i = 0; i < s_rssi_count; i++) {
    uint8_t offset = (uint8_t)(STATUS_SAMPLES - s_rssi_count + i);
    uint8_t index = (uint8_t)((s_rssi_cursor + offset) % STATUS_SAMPLES);
    int value = signal_percent(s_rssi_history[index]);
    int height = value * 75 / 100;
    int x = 417 + i * 8;
    if (height > 0)
      hermes_gfx_fill_rect(&s_gfx, x, 136 - height, 5, height, COL_FOCUS);
  }
  hermes_gfx_text(&s_gfx, 415, 138, "-90", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 598, 46, "-30", COL_DIM, 1);

  draw_footer("PWR short = menu");
}

static void draw_settings(void) {
  hermes_gfx_text(&s_gfx, 12, 6, "// DISPLAY SETTINGS", COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, 12, 18, GFX_W - 24, COL_LINE);

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    int y = 38 + i * 54;
    bool selected = i == s_settings_index;
    if (selected)
      hermes_gfx_rect(&s_gfx, 12, y - 8, GFX_W - 24, 43, COL_FOCUS);
    char label[32];
    snprintf(label, sizeof(label), "%s %s", selected ? ">" : " ",
             i == 0 ? "DISPLAY THEME" : "BRIGHTNESS");
    hermes_gfx_text(&s_gfx, 22, y, label, selected ? COL_FOCUS : COL_TEXT, 1);
    char value[40];
    if (i == 0) {
      snprintf(value, sizeof(value), "[%s]", hermes_theme_name(s_theme));
      hermes_gfx_text(&s_gfx, 210, y - 3, value, selected ? COL_FOCUS : COL_DIM,
                      2);
    } else {
      snprintf(value, sizeof(value), "[%u%%]", (unsigned)s_brightness);
      hermes_gfx_text(&s_gfx, 210, y - 3, value, selected ? COL_FOCUS : COL_DIM,
                      2);
      draw_progress(320, y, 280, s_brightness, COL_FOCUS);
    }
  }
  hermes_gfx_text(&s_gfx, 22, 137, "saved internally in NVS flash", COL_DIM, 1);
  draw_footer("BOOT next   TAP change   PWR menu");
}

static void draw_wifi(void) {
  hermes_gfx_text(&s_gfx, 12, 6, "// WIFI ROUTES", COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, 12, 18, GFX_W - 24, COL_LINE);
  int active = hermes_wifi_active_index();

  for (int i = 0; i < hermes_wifi_count(); i++) {
    const hermes_wifi_net_t *network = hermes_wifi_net(i);
    hermes_wifi_sight_t sight = {0};
    bool has_sight = hermes_wifi_sight(i, &sight);
    bool selected = i == s_wifi_index;
    bool is_active = i == active;
    bool visible = has_sight && sight.visible;
    int y = 30 + i * 38;
    char left[48];
    snprintf(left, sizeof(left), "%s [%c] %s", selected ? ">" : " ",
             is_active ? '*' : ' ', network->ssid);
    hermes_gfx_text(&s_gfx, 14, y, left, selected ? COL_FOCUS : COL_TEXT, 1);
    hermes_gfx_text(&s_gfx, 38, y + 12, network->label, COL_DIM, 1);

    char right[40];
    if (is_active && hermes_wifi_is_connected()) {
      snprintf(right, sizeof(right), "%ddBm  ACTIVE", hermes_wifi_rssi());
      hermes_gfx_text(&s_gfx, GFX_W - 14 - hermes_gfx_text_width(right, 1), y,
                      right, COL_OK, 1);
    } else if (visible) {
      snprintf(right, sizeof(right), "%ddBm  ch%u", sight.rssi,
               (unsigned)sight.channel);
      hermes_gfx_text(&s_gfx, GFX_W - 14 - hermes_gfx_text_width(right, 1), y,
                      right, COL_DIM, 1);
    } else {
      snprintf(right, sizeof(right), "NOT IN RANGE");
      hermes_gfx_text(&s_gfx, GFX_W - 14 - hermes_gfx_text_width(right, 1), y,
                      right, COL_BAD, 1);
    }
  }

  if (s_wifi_result[0])
    hermes_gfx_text(&s_gfx, 14, 142, s_wifi_result, COL_WARN, 1);
  else if (s_wifi_scanning)
    hermes_gfx_text(&s_gfx, 14, 142, "scanning saved routes...", COL_DIM, 1);
  else if (s_wifi_busy)
    hermes_gfx_text(&s_gfx, 14, 142, "changing route...", COL_DIM, 1);
  draw_footer("BOOT next   TAP connect   PWR menu");
}

static void draw_list_heading(const char *title) {
  hermes_gfx_text(&s_gfx, 12, 6, title, COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, 12, 18, GFX_W - 24, COL_LINE);
}

static const char *git_repo(void) { return GIT_REPOS[s_git_repo_index]; }

static void draw_git_heading(const char *section) {
  char title[72];
  if (!section || !section[0])
    snprintf(title, sizeof(title), "// GIT :: %s", git_repo());
  else
    snprintf(title, sizeof(title), "// GIT :: %s :: %s", git_repo(), section);
  draw_list_heading(title);
}

static void draw_simple_list(const char *const *items, int count, int selected,
                             int first_y, int row_height, int scale) {
  for (int i = 0; i < count; i++) {
    char row[72];
    snprintf(row, sizeof(row), "%s %s", i == selected ? ">" : " ", items[i]);
    hermes_gfx_text(&s_gfx, 18, first_y + i * row_height, row,
                    i == selected ? COL_FOCUS : COL_TEXT, scale);
  }
}

static void draw_git(void) {
  draw_list_heading("// GIT");
  draw_simple_list(GIT_REPOS, GIT_REPO_COUNT, s_git_repo_index, 39, 42, 2);
  draw_footer("BOOT next   TAP select   PWR back");
}

static void draw_git_menu(void) {
  draw_git_heading(NULL);
  draw_simple_list(GIT_MENU, GIT_MENU_COUNT, s_git_index, 39, 42, 2);
  draw_footer("BOOT next   TAP select   PWR back");
}

static void draw_workflows(void) {
  draw_git_heading("WORKFLOWS");
  draw_simple_list(WORKFLOWS, WORKFLOW_COUNT, s_workflow_index, 39, 42, 2);
  draw_footer("BOOT next   TAP select   PWR back");
}

static void draw_git_loading(void) {
  draw_git_heading(NULL);
  hermes_gfx_text(&s_gfx, 18, 44, s_git_loading_title, COL_FOCUS, 2);
  hermes_gfx_text(&s_gfx, 18, 86, "working...", COL_DIM, 1);
  draw_footer("PWR back");
}

static void draw_git_branches(void) {
  draw_git_heading("BRANCHES 48h");
  if (s_git_branch_count <= 0) {
    hermes_gfx_text(&s_gfx, 18, 48, "no branches active in 48h", COL_WARN, 1);
  } else {
    for (int i = 0; i < s_git_branch_count; i++) {
      char row[72];
      snprintf(row, sizeof(row), "%s %s", i == s_git_branch_index ? ">" : " ",
               s_git_branches[i].name);
      hermes_gfx_text(&s_gfx, 18, 28 + i * 16, row,
                      i == s_git_branch_index ? COL_FOCUS : COL_TEXT, 1);
    }
  }
  draw_footer("BOOT next   TAP create   PWR back");
}

static void draw_git_pulls(void) {
  draw_git_heading("MERGE");
  if (s_git_pull_count <= 0) {
    hermes_gfx_text(&s_gfx, 18, 48, "no open console pulls", COL_WARN, 1);
  } else {
    for (int i = 0; i < s_git_pull_count; i++) {
      char row[80];
      snprintf(row, sizeof(row), "%s #%d %s <- %s",
               i == s_git_pull_index ? ">" : " ", s_git_pulls[i].number,
               s_git_pulls[i].base, s_git_pulls[i].head);
      hermes_gfx_text(&s_gfx, 18, 28 + i * 16, row,
                      i == s_git_pull_index ? COL_FOCUS : COL_TEXT, 1);
    }
  }
  draw_footer("BOOT next   TAP open   PWR back");
}

static void draw_git_detail(void) {
  char line[80];
  draw_git_heading("PULL");
  snprintf(line, sizeof(line), "#%d  %s <- %s", s_git_detail.number,
           s_git_detail.base, s_git_detail.head);
  hermes_gfx_text(&s_gfx, 18, 28, line, COL_FOCUS, 1);
  hermes_gfx_text(&s_gfx, 18, 46, s_git_detail.title, COL_TEXT, 1);
  snprintf(line, sizeof(line), "state  %s", s_git_detail.mergeable_state);
  hermes_gfx_text(&s_gfx, 18, 66, line, COL_DIM, 1);
  snprintf(line, sizeof(line), "approved  %s",
           s_git_detail.approved_by[0] ? s_git_detail.approved_by : "none");
  hermes_gfx_text(&s_gfx, 18, 82, line, COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 18, 104,
                  s_git_detail.ready ? "> merge" : "merge locked",
                  s_git_detail.ready ? COL_FOCUS : COL_WARN, 2);
  draw_footer(s_git_detail.ready ? "TAP merge   PWR back"
                                 : "waiting reviews/checks   PWR back");
}

static void draw_git_confirm(void) {
  draw_git_heading("CONFIRM MERGE");
  char line[80];
  snprintf(line, sizeof(line), "#%d  %s <- %s", s_git_detail.number,
           s_git_detail.base, s_git_detail.head);
  hermes_gfx_text(&s_gfx, 18, 28, line, COL_DIM, 1);
  draw_simple_list(GIT_CONFIRM, GIT_CONFIRM_COUNT, s_git_confirm_index, 56,
                   32, 2);
  draw_footer("BOOT next   TAP select   PWR back");
}

static void draw_git_result(void) {
  draw_git_heading("RESULT");
  hermes_gfx_text(&s_gfx, 18, 40, s_git_result_title, COL_FOCUS, 2);
  if (s_git_result_detail[0])
    hermes_gfx_text(&s_gfx, 18, 78, s_git_result_detail, COL_DIM, 1);
  draw_footer("TAP or PWR back");
}

static void render_lcd(void) {
  hermes_gfx_clear(&s_gfx, COL_BG);
  switch (s_screen) {
  case SCR_MENU:
    draw_menu();
    break;
  case SCR_GIT:
    draw_git();
    break;
  case SCR_GIT_MENU:
    draw_git_menu();
    break;
  case SCR_WORKFLOWS:
    draw_workflows();
    break;
  case SCR_GIT_LOADING:
    draw_git_loading();
    break;
  case SCR_GIT_BRANCHES:
    draw_git_branches();
    break;
  case SCR_GIT_PULLS:
    draw_git_pulls();
    break;
  case SCR_GIT_DETAIL:
    draw_git_detail();
    break;
  case SCR_GIT_CONFIRM:
    draw_git_confirm();
    break;
  case SCR_GIT_RESULT:
    draw_git_result();
    break;
  case SCR_STATUS:
    draw_status();
    break;
  case SCR_SETTINGS:
    draw_settings();
    break;
  case SCR_WIFI:
    draw_wifi();
    break;
  }
  hermes_gfx_flush(&s_gfx);
}

static void move_down(void) {
  switch (s_screen) {
  case SCR_MENU:
    s_menu_index = (s_menu_index + 1) % MENU_COUNT;
    break;
  case SCR_GIT:
    s_git_repo_index = (s_git_repo_index + 1) % GIT_REPO_COUNT;
    break;
  case SCR_GIT_MENU:
    s_git_index = (s_git_index + 1) % GIT_MENU_COUNT;
    break;
  case SCR_WORKFLOWS:
    s_workflow_index = (s_workflow_index + 1) % WORKFLOW_COUNT;
    break;
  case SCR_GIT_BRANCHES:
    if (s_git_branch_count > 0)
      s_git_branch_index = (s_git_branch_index + 1) % s_git_branch_count;
    else
      return;
    break;
  case SCR_GIT_PULLS:
    if (s_git_pull_count > 0)
      s_git_pull_index = (s_git_pull_index + 1) % s_git_pull_count;
    else
      return;
    break;
  case SCR_GIT_CONFIRM:
    s_git_confirm_index = (s_git_confirm_index + 1) % GIT_CONFIRM_COUNT;
    break;
  case SCR_GIT_LOADING:
  case SCR_GIT_DETAIL:
  case SCR_GIT_RESULT:
    return;
  case SCR_SETTINGS:
    s_settings_index = (s_settings_index + 1) % SETTINGS_COUNT;
    break;
  case SCR_WIFI:
    s_wifi_index = (s_wifi_index + 1) % hermes_wifi_count();
    break;
  case SCR_STATUS:
    return;
  }
  note_activity();
  mark_dirty();
}

static void go_back(void) {
  s_navigation_generation++;
  switch (s_screen) {
  case SCR_MENU:
    break;
  case SCR_GIT:
  case SCR_STATUS:
  case SCR_SETTINGS:
  case SCR_WIFI:
    s_screen = SCR_MENU;
    break;
  case SCR_GIT_MENU:
    s_screen = SCR_GIT;
    break;
  case SCR_WORKFLOWS:
    s_screen = SCR_GIT_MENU;
    break;
  case SCR_GIT_LOADING:
  case SCR_GIT_RESULT:
    s_screen = s_git_back;
    break;
  case SCR_GIT_BRANCHES:
    s_screen = SCR_WORKFLOWS;
    break;
  case SCR_GIT_PULLS:
    s_screen = SCR_GIT_MENU;
    break;
  case SCR_GIT_DETAIL:
    s_screen = SCR_GIT_PULLS;
    break;
  case SCR_GIT_CONFIRM:
    s_screen = SCR_GIT_DETAIL;
    break;
  }
  hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
  note_activity();
  mark_dirty();
}

static void wifi_open_task(void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  esp_err_t result = hermes_wifi_refresh_scan();
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  if (generation == s_navigation_generation && s_screen == SCR_WIFI) {
    s_wifi_index = hermes_wifi_active_index();
    s_wifi_result[0] = 0;
    if (result != ESP_OK)
      snprintf(s_wifi_result, sizeof(s_wifi_result),
               "scan unavailable; saved routes shown");
    hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
    mark_dirty();
  }
  s_wifi_scanning = false;
  mark_dirty();
  if (s_lock)
    xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "Wi-Fi scan worker finished: %s; stack watermark=%u",
           esp_err_to_name(result),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  vTaskDelete(NULL);
}

static void wifi_select_task(void *argument) {
  int index = (int)(intptr_t)argument;
  esp_err_t result = hermes_wifi_select(index);
  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  if (result == ESP_OK) {
    snprintf(s_wifi_result, sizeof(s_wifi_result), "route active: %s",
             hermes_wifi_net(index)->ssid);
  } else if (result == ESP_ERR_NOT_FOUND) {
    snprintf(s_wifi_result, sizeof(s_wifi_result), "route not in range");
  } else {
    snprintf(s_wifi_result, sizeof(s_wifi_result),
             "connection failed; previous route restored");
  }
  s_wifi_busy = false;
  hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
  mark_dirty();
  if (s_lock)
    xSemaphoreGive(s_lock);
  ESP_LOGI(TAG, "Wi-Fi selection worker finished: %s; stack watermark=%u",
           esp_err_to_name(result),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  vTaskDelete(NULL);
}

static void open_wifi(void) {
  if (s_wifi_busy) {
    s_screen = SCR_WIFI;
    snprintf(s_wifi_result, sizeof(s_wifi_result), "connection in progress");
    hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_THINKING, now_ms());
    mark_dirty();
    return;
  }
  if (s_wifi_scanning) {
    s_screen = SCR_WIFI;
    mark_dirty();
    return;
  }
  hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_THINKING, now_ms());
  s_wifi_scanning = true;
  uint32_t generation = ++s_navigation_generation;
  s_screen = SCR_WIFI;
  s_wifi_index = hermes_wifi_active_index();
  s_wifi_result[0] = 0;
  mark_dirty();
  if (xTaskCreate(wifi_open_task, "wifi_scan_ui", WIFI_WORKER_STACK_BYTES,
                  (void *)(uintptr_t)generation, 4, NULL) != pdPASS) {
    snprintf(s_wifi_result, sizeof(s_wifi_result), "could not start scan");
    s_wifi_scanning = false;
    hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
    mark_dirty();
  }
}

static void activate_menu_index(int index) {
  switch (index) {
  case 0:
    s_navigation_generation++;
    s_screen = SCR_GIT;
    s_git_repo_index = 0;
    s_git_index = 0;
    note_activity();
    mark_dirty();
    break;
  case 1:
    s_navigation_generation++;
    s_screen = SCR_STATUS;
    mark_dirty();
    break;
  case 2:
    s_navigation_generation++;
    s_screen = SCR_SETTINGS;
    s_settings_index = 0;
    mark_dirty();
    break;
  case 3:
    open_wifi();
    break;
  case 4:
    boot_screen(true);
    break;
  }
}

static void show_git_result(const char *title, const char *detail) {
  snprintf(s_git_result_title, sizeof(s_git_result_title), "%s", title);
  snprintf(s_git_result_detail, sizeof(s_git_result_detail), "%s",
           detail ? detail : "");
  s_screen = SCR_GIT_RESULT;
  hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
  mark_dirty();
}

static void git_worker(void *argument) {
  uint32_t generation = (uint32_t)(uintptr_t)argument;
  git_job_t job = s_git_job;
  char repo[24];
  char branch[HERMES_GIT_NAME_LEN];
  char key[40];
  char sha[HERMES_GIT_SHA_LEN];
  int number = s_git_detail.number;
  snprintf(repo, sizeof(repo), "%s", git_repo());
  snprintf(branch, sizeof(branch), "%s", s_git_job_branch);
  snprintf(key, sizeof(key), "%s", s_git_job_key);
  snprintf(sha, sizeof(sha), "%s", s_git_detail.head_sha);

  char err[HERMES_GIT_MSG_LEN];
  err[0] = 0;
  hermes_git_branch_t branches[HERMES_GIT_MAX_BRANCHES];
  hermes_git_pull_t pulls[HERMES_GIT_MAX_PULLS];
  hermes_git_creation_t created = {0};
  hermes_git_merge_t merged = {0};
  int count = 0;
  esp_err_t result = ESP_FAIL;

  switch (job) {
  case GIT_JOB_BRANCHES:
    result = hermes_actions_list_branches(repo, branches, &count, err,
                                           sizeof(err));
    break;
  case GIT_JOB_CREATE_PROD:
    result = hermes_actions_create_prod(repo, key, &created, err, sizeof(err));
    break;
  case GIT_JOB_CREATE_BRANCH:
    result = hermes_actions_create_from_branch(repo, branch, key, &created, err,
                                                sizeof(err));
    break;
  case GIT_JOB_PULLS:
    result = hermes_actions_list_pulls(repo, pulls, &count, err, sizeof(err));
    break;
  case GIT_JOB_MERGE:
    result = hermes_actions_merge(repo, number, sha, key, &merged, err,
                                 sizeof(err));
    break;
  }

  if (s_lock)
    xSemaphoreTake(s_lock, portMAX_DELAY);
  s_git_busy = false;
  if (generation == s_navigation_generation && s_screen == SCR_GIT_LOADING) {
    if (result != ESP_OK) {
      show_git_result(err[0] ? err : "service offline", NULL);
    } else if (job == GIT_JOB_BRANCHES) {
      memcpy(s_git_branches, branches, sizeof(branches));
      s_git_branch_count = count;
      s_git_branch_index = 0;
      if (count == 0)
        show_git_result("no branches active in 48h", NULL);
      else {
        s_screen = SCR_GIT_BRANCHES;
        hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
        mark_dirty();
      }
    } else if (job == GIT_JOB_PULLS) {
      memcpy(s_git_pulls, pulls, sizeof(pulls));
      s_git_pull_count = count;
      s_git_pull_index = 0;
      if (count == 0)
        show_git_result("no open console pulls", NULL);
      else {
        s_screen = SCR_GIT_PULLS;
        hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
        mark_dirty();
      }
    } else if (job == GIT_JOB_MERGE) {
      show_git_result(merged.merged ? "merged" : "merge failed",
                     merged.message);
    } else {
      char title[48];
      snprintf(title, sizeof(title), "%s #%d",
               created.created ? "opened" : "reused", created.pull.number);
      show_git_result(title, created.warnings[0] ? created.warnings : created.pull.title);
    }
  } else {
    hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
    mark_dirty();
  }
  if (s_lock)
    xSemaphoreGive(s_lock);
  vTaskDelete(NULL);
}

static void start_git_job(git_job_t job, const char *title, screen_t back,
                           hermes_avatar_state_t avatar) {
  if (s_git_busy)
    return;
  if (!hermes_wifi_is_connected()) {
    s_git_back = back;
    show_git_result("service offline", NULL);
    return;
  }
  if (!hermes_actions_configured()) {
    s_git_back = back;
    show_git_result("service offline", NULL);
    return;
  }
  s_git_busy = true;
  s_git_job = job;
  s_git_back = back;
  snprintf(s_git_loading_title, sizeof(s_git_loading_title), "%s", title);
  hermes_actions_make_key(s_git_job_key, sizeof(s_git_job_key));
  hermes_avatar_set_state(&s_avatar, avatar, now_ms());
  uint32_t generation = ++s_navigation_generation;
  s_screen = SCR_GIT_LOADING;
  mark_dirty();
  if (xTaskCreate(git_worker, "git_ui", GIT_WORKER_STACK_BYTES,
                  (void *)(uintptr_t)generation, 4, NULL) != pdPASS) {
    s_git_busy = false;
    show_git_result("service offline", "could not start worker");
  }
}

static void activate_git(void) {
  s_navigation_generation++;
  s_screen = SCR_GIT_MENU;
  s_git_index = 0;
  mark_dirty();
}

static void activate_git_menu(void) {
  if (s_git_index == 0) {
    s_navigation_generation++;
    s_screen = SCR_WORKFLOWS;
    s_workflow_index = 0;
    mark_dirty();
  } else {
    start_git_job(GIT_JOB_PULLS, "merge", SCR_GIT_MENU,
                  HERMES_AVATAR_STATE_THINKING);
  }
}

static void activate_workflow(void) {
  if (s_workflow_index == 0) {
    start_git_job(GIT_JOB_BRANCHES, "git [main <- branch]", SCR_WORKFLOWS,
                  HERMES_AVATAR_STATE_THINKING);
  } else {
    start_git_job(GIT_JOB_CREATE_PROD, "git [prod <- main]", SCR_WORKFLOWS,
                  HERMES_AVATAR_STATE_BUSY);
  }
}

static void activate_git_branch(void) {
  if (s_git_branch_count <= 0)
    return;
  snprintf(s_git_job_branch, sizeof(s_git_job_branch), "%s",
           s_git_branches[s_git_branch_index].name);
  start_git_job(GIT_JOB_CREATE_BRANCH, s_git_job_branch, SCR_GIT_BRANCHES,
                HERMES_AVATAR_STATE_BUSY);
}

static void activate_git_pull(void) {
  if (s_git_pull_count <= 0)
    return;
  s_git_detail = s_git_pulls[s_git_pull_index];
  s_navigation_generation++;
  s_screen = SCR_GIT_DETAIL;
  mark_dirty();
}

static void activate_git_detail(void) {
  if (!s_git_detail.ready)
    return;
  s_git_confirm_index = 0;
  s_navigation_generation++;
  s_screen = SCR_GIT_CONFIRM;
  mark_dirty();
}

static void activate_git_confirm(void) {
  if (s_git_confirm_index == 1) {
    go_back();
    return;
  }
  start_git_job(GIT_JOB_MERGE, "confirm merge", SCR_GIT_CONFIRM,
                HERMES_AVATAR_STATE_BUSY);
}

static void activate_settings(void) {
  if (s_settings_index == 0) {
    s_theme = (hermes_theme_t)((s_theme + 1) % THEME_COUNT);
    hermes_theme_apply(s_theme);
  } else {
    if (s_brightness >= 95)
      s_brightness = 5;
    else
      s_brightness = (uint8_t)(s_brightness + 15);
    if (s_brightness > 100)
      s_brightness = 100;
    hermes_display_set_brightness(s_brightness);
  }
  hermes_settings_save(s_theme, s_brightness);
  mark_dirty();
}

static void activate_wifi(void) {
  if (s_wifi_busy)
    return;
  if (!hermes_wifi_is_selectable(s_wifi_index)) {
    snprintf(s_wifi_result, sizeof(s_wifi_result), "%s is not in range",
             hermes_wifi_net(s_wifi_index)->ssid);
    mark_dirty();
    return;
  }
  s_wifi_busy = true;
  hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_THINKING, now_ms());
  s_wifi_result[0] = 0;
  mark_dirty();
  if (xTaskCreate(wifi_select_task, "wifi_select_ui", WIFI_WORKER_STACK_BYTES,
                  (void *)(intptr_t)s_wifi_index, 4, NULL) != pdPASS) {
    s_wifi_busy = false;
    hermes_avatar_set_state(&s_avatar, HERMES_AVATAR_STATE_IDLE, now_ms());
    snprintf(s_wifi_result, sizeof(s_wifi_result),
             "could not start connection");
    mark_dirty();
  }
}

/* BOOT moves the highlight. A tap activates that highlight only.
 * Never map tap x,y onto a row — the AXS15231B is not a menu hit-test. */
static void handle_tap(uint16_t x, uint16_t y) {
  if ((s_wifi_scanning || s_wifi_busy) && s_screen == SCR_WIFI)
    return;
  if (s_git_busy && s_screen == SCR_GIT_LOADING)
    return;
  note_activity();
  switch (s_screen) {
  case SCR_MENU:
    ESP_LOGI(TAG, "menu select %d (%s); tap=%u,%u", s_menu_index,
             MENU[s_menu_index], (unsigned)x, (unsigned)y);
    activate_menu_index(s_menu_index);
    break;
  case SCR_GIT:
    activate_git();
    break;
  case SCR_GIT_MENU:
    activate_git_menu();
    break;
  case SCR_WORKFLOWS:
    activate_workflow();
    break;
  case SCR_GIT_LOADING:
    break;
  case SCR_GIT_BRANCHES:
    activate_git_branch();
    break;
  case SCR_GIT_PULLS:
    activate_git_pull();
    break;
  case SCR_GIT_DETAIL:
    activate_git_detail();
    break;
  case SCR_GIT_CONFIRM:
    activate_git_confirm();
    break;
  case SCR_GIT_RESULT:
    go_back();
    break;
  case SCR_SETTINGS:
    activate_settings();
    break;
  case SCR_WIFI:
    activate_wifi();
    break;
  case SCR_STATUS:
    break;
  }
}

static void process_button_edges(button_t *boot, bool *boot_armed,
                                 TickType_t *boot_last_tick, button_t *power,
                                 bool *power_armed, TickType_t now_tick) {
  input_edge_t edge;
  while (xQueueReceive(s_input_queue, &edge, 0) == pdTRUE) {
    if (edge.pin == HERMES_BOOT_PIN) {
      if (edge.level) {
        *boot_armed = true;
      } else if (*boot_armed && edge.tick - *boot_last_tick >=
                                    pdMS_TO_TICKS(BTN_DEBOUNCE_MS)) {
        *boot_last_tick = edge.tick;
        if (s_lock)
          xSemaphoreTake(s_lock, portMAX_DELAY);
        move_down();
        if (s_lock)
          xSemaphoreGive(s_lock);
      }
      boot->previous_level = edge.level;
      continue;
    }

    if (!*power_armed) {
      if (edge.level)
        *power_armed = true;
      power->previous_level = edge.level;
      continue;
    }
    if (!edge.level && power->previous_level) {
      power->down_tick = edge.tick;
      power->long_fired = false;
    } else if (edge.level && !power->previous_level && !power->long_fired) {
      if (s_lock)
        xSemaphoreTake(s_lock, portMAX_DELAY);
      if (edge.tick - power->down_tick >= pdMS_TO_TICKS(PWR_HOLD_MS)) {
        power->long_fired = true;
        boot_screen(true);
      } else {
        go_back();
        if (s_lock)
          xSemaphoreGive(s_lock);
      }
    }
    power->previous_level = edge.level;
  }

  if (*power_armed && !power->previous_level && !power->long_fired &&
      now_tick - power->down_tick >= pdMS_TO_TICKS(PWR_HOLD_MS)) {
    power->long_fired = true;
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    boot_screen(true);
  }
}

static void ui_task(void *argument) {
  (void)argument;
  bool touch_down = false;
  uint8_t touch_sample_count = 0;
  uint16_t touch_x = 0;
  uint16_t touch_y = 0;

  button_t boot = {.previous_level = gpio_get_level(HERMES_BOOT_PIN)};
  button_t power = {.previous_level = gpio_get_level(HERMES_PWR_PIN)};
  bool boot_armed = boot.previous_level;
  bool power_armed = power.previous_level;
  TickType_t boot_last_tick = 0;

  while (true) {
    process_button_edges(&boot, &boot_armed, &boot_last_tick, &power,
                         &power_armed, xTaskGetTickCount());
    if (s_input_queue_overflows) {
      ESP_LOGW(TAG, "input queue overflowed %lu time(s)",
               (unsigned long)s_input_queue_overflows);
      s_input_queue_overflows = 0;
    }

    bool touched = false;
    uint16_t tx = 0;
    uint16_t ty = 0;
    if (hermes_touch_read(&touched, &tx, &ty)) {
      if (touched) {
        if (!touch_down) {
          touch_down = true;
          touch_sample_count = 0;
        }
        if (touch_sample_count < UINT8_MAX)
          touch_sample_count++;
        touch_x = tx;
        touch_y = ty;
      } else if (touch_down) {
        touch_down = false;
        if (touch_sample_count >= TOUCH_MIN_SAMPLES) {
          if (s_lock)
            xSemaphoreTake(s_lock, portMAX_DELAY);
          handle_tap(touch_x, touch_y);
          if (s_lock)
            xSemaphoreGive(s_lock);
        } else
          ESP_LOGW(TAG, "tap rejected: only %u touch sample(s)",
                   (unsigned)touch_sample_count);
      }
    }

    update_status_sample(now_ms());
    if (s_lock)
      xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t frame_now = now_ms();
    bool avatar_active = s_screen == SCR_MENU;
    if (avatar_active && hermes_avatar_tick(&s_avatar, frame_now))
      mark_dirty();
    if (s_dirty) {
      s_dirty = false;
      render_lcd();
    }
    if (s_lock)
      xSemaphoreGive(s_lock);

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

esp_err_t hermes_ui_start(void) {
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock)
    return ESP_ERR_NO_MEM;

  hermes_settings_load(&s_theme, &s_brightness);
  hermes_theme_apply(s_theme);

  ESP_ERROR_CHECK(hermes_i2c_init());
  ESP_ERROR_CHECK(hermes_power_hold_enable());
  ESP_ERROR_CHECK(hermes_display_init());
  hermes_display_set_brightness(s_brightness);
  if (hermes_touch_init() != ESP_OK)
    ESP_LOGW(TAG, "touch controller unavailable");
  if (!hermes_gfx_init(&s_gfx))
    return ESP_ERR_NO_MEM;
  adc_init();
  hermes_avatar_init(&s_avatar, now_ms());

  boot_screen(false);
  esp_err_t input_err = input_init();
  if (input_err != ESP_OK) {
    ESP_LOGE(TAG, "button input init failed: %s", esp_err_to_name(input_err));
    return input_err;
  }
  s_screen = SCR_MENU;
  s_menu_index = 0;
  update_status_sample(now_ms());
  render_lcd();
  if (xTaskCreate(ui_task, "hermes_ui", 8192, NULL, 5, NULL) != pdPASS)
    return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG, "standalone Hermes UI ready");
  return ESP_OK;
}
