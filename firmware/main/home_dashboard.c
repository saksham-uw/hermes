#include "home_dashboard.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "hermes_i2c.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "home_clock";
#define RTC_ADDRESS 0x51
#define RTC_SECONDS_REGISTER 0x04
#define FOCUS_SECONDS (25U * 60U)
#define SHORT_BREAK_SECONDS (5U * 60U)
#define LONG_BREAK_SECONDS (15U * 60U)
#define POMO_MAGIC 0x504f4d32U

typedef struct {
  uint32_t magic;
  uint8_t kind;
  uint8_t state;
  uint8_t completed;
  uint8_t reserved;
  uint32_t remaining;
  int64_t deadline_epoch;
} persisted_pomo_t;

static i2c_master_dev_handle_t s_rtc;
static home_pomo_kind_t s_kind = HOME_POMO_FOCUS;
static home_pomo_state_t s_state = HOME_POMO_READY;
static uint8_t s_completed;
static uint32_t s_remaining = FOCUS_SECONDS;
static int64_t s_deadline_us;
static int64_t s_deadline_epoch;
static bool s_just_completed;
static uint32_t s_last_second = UINT32_MAX;

static uint8_t bcd_to_u8(uint8_t value) {
  return (uint8_t)((value >> 4) * 10U + (value & 0x0f));
}

static uint8_t u8_to_bcd(uint8_t value) {
  return (uint8_t)(((value / 10U) << 4) | (value % 10U));
}

static bool trusted_time(void) {
  return time(NULL) >= 1704067200; /* 2024-01-01 */
}

static uint32_t duration_for(home_pomo_kind_t kind) {
  if (kind == HOME_POMO_SHORT_BREAK)
    return SHORT_BREAK_SECONDS;
  if (kind == HOME_POMO_LONG_BREAK)
    return LONG_BREAK_SECONDS;
  return FOCUS_SECONDS;
}

static void persist_pomodoro(void) {
  persisted_pomo_t value = {
      .magic = POMO_MAGIC,
      .kind = (uint8_t)s_kind,
      .state = (uint8_t)s_state,
      .completed = s_completed,
      .remaining = s_remaining,
      .deadline_epoch = s_deadline_epoch,
  };
  nvs_handle_t handle;
  if (nvs_open("home", NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_set_blob(handle, "pomodoro", &value, sizeof(value));
  nvs_commit(handle);
  nvs_close(handle);
}

static void prepare_next_interval(void) {
  if (s_kind == HOME_POMO_FOCUS) {
    s_completed++;
    if (s_completed >= 4) {
      s_kind = HOME_POMO_LONG_BREAK;
    } else {
      s_kind = HOME_POMO_SHORT_BREAK;
    }
  } else {
    if (s_kind == HOME_POMO_LONG_BREAK)
      s_completed = 0;
    s_kind = HOME_POMO_FOCUS;
  }
  s_state = HOME_POMO_READY;
  s_remaining = duration_for(s_kind);
  s_deadline_us = 0;
  s_deadline_epoch = 0;
  s_just_completed = true;
  persist_pomodoro();
}

static void restore_pomodoro(void) {
  nvs_handle_t handle;
  persisted_pomo_t value = {0};
  size_t size = sizeof(value);
  if (nvs_open("home", NVS_READONLY, &handle) != ESP_OK)
    return;
  esp_err_t result = nvs_get_blob(handle, "pomodoro", &value, &size);
  nvs_close(handle);
  if (result != ESP_OK || size != sizeof(value) || value.magic != POMO_MAGIC ||
      value.kind > HOME_POMO_LONG_BREAK || value.state > HOME_POMO_PAUSED ||
      value.completed > 4 || value.remaining > FOCUS_SECONDS)
    return;
  s_kind = (home_pomo_kind_t)value.kind;
  s_state = (home_pomo_state_t)value.state;
  s_completed = value.completed;
  s_remaining = value.remaining;
  s_deadline_epoch = value.deadline_epoch;
  if (s_state != HOME_POMO_RUNNING)
    return;
  if (trusted_time() && s_deadline_epoch > time(NULL)) {
    s_remaining = (uint32_t)(s_deadline_epoch - time(NULL));
    s_deadline_us = esp_timer_get_time() + (int64_t)s_remaining * 1000000LL;
  } else if (trusted_time() && s_deadline_epoch > 0) {
    prepare_next_interval();
  } else {
    s_state = HOME_POMO_PAUSED;
    s_deadline_epoch = 0;
    persist_pomodoro();
  }
}

static time_t utc_epoch(int year, int month, int day, int hour, int minute,
                        int second) {
  struct tm value = {
      .tm_year = year - 1900,
      .tm_mon = month - 1,
      .tm_mday = day,
      .tm_hour = hour,
      .tm_min = minute,
      .tm_sec = second,
      .tm_isdst = 0,
  };
  char *old_tz = getenv("TZ");
  char saved[64] = {0};
  if (old_tz)
    snprintf(saved, sizeof(saved), "%s", old_tz);
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t result = mktime(&value);
  setenv("TZ", saved[0] ? saved : "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  return result;
}

static void read_rtc(void) {
  if (!s_rtc)
    return;
  uint8_t reg = RTC_SECONDS_REGISTER;
  uint8_t data[7] = {0};
  if (i2c_master_transmit_receive(s_rtc, &reg, 1, data, sizeof(data), 100) != ESP_OK)
    return;
  if (data[0] & 0x80) {
    ESP_LOGW(TAG, "RTC oscillator-stop flag set; waiting for network time");
    return;
  }
  int year = 2000 + bcd_to_u8(data[6]);
  int month = bcd_to_u8(data[5] & 0x1f);
  int day = bcd_to_u8(data[3] & 0x3f);
  int hour = bcd_to_u8(data[2] & 0x3f);
  int minute = bcd_to_u8(data[1] & 0x7f);
  int second = bcd_to_u8(data[0] & 0x7f);
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 59)
    return;
  time_t epoch = utc_epoch(year, month, day, hour, minute, second);
  struct timeval tv = {.tv_sec = epoch};
  settimeofday(&tv, NULL);
  ESP_LOGI(TAG, "system time restored from PCF85063 RTC");
}

static void write_rtc(struct timeval *tv) {
  if (!s_rtc || !tv)
    return;
  struct tm utc;
  gmtime_r(&tv->tv_sec, &utc);
  uint8_t data[8] = {
      RTC_SECONDS_REGISTER,
      u8_to_bcd((uint8_t)utc.tm_sec),
      u8_to_bcd((uint8_t)utc.tm_min),
      u8_to_bcd((uint8_t)utc.tm_hour),
      u8_to_bcd((uint8_t)utc.tm_mday),
      u8_to_bcd((uint8_t)utc.tm_wday),
      u8_to_bcd((uint8_t)(utc.tm_mon + 1)),
      u8_to_bcd((uint8_t)((utc.tm_year + 1900) % 100)),
  };
  esp_err_t result = i2c_master_transmit(s_rtc, data, sizeof(data), 100);
  ESP_LOGI(TAG, "network time synced; RTC write: %s", esp_err_to_name(result));
}

esp_err_t home_dashboard_init(void) {
  setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  esp_err_t rtc_result = hermes_i2c_add_dev(hermes_i2c_sys_bus(), RTC_ADDRESS,
                                             100000, &s_rtc);
  if (rtc_result == ESP_OK)
    read_rtc();
  else
    ESP_LOGW(TAG, "PCF85063 RTC unavailable: %s", esp_err_to_name(rtc_result));
  restore_pomodoro();
  return ESP_OK;
}

esp_err_t home_dashboard_start_network_time(void) {
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  config.sync_cb = write_rtc;
  esp_err_t result = esp_netif_sntp_init(&config);
  if (result != ESP_OK)
    ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(result));
  return result;
}

bool home_dashboard_local_time(struct tm *out) {
  if (!out || !trusted_time())
    return false;
  time_t now = time(NULL);
  return localtime_r(&now, out) != NULL;
}

void home_dashboard_pomodoro_boot(void) {
  s_just_completed = false;
  if (s_state == HOME_POMO_READY) {
    s_state = HOME_POMO_RUNNING;
    s_deadline_us = esp_timer_get_time() + (int64_t)s_remaining * 1000000LL;
    s_deadline_epoch = trusted_time() ? time(NULL) + s_remaining : 0;
  } else if (s_state == HOME_POMO_RUNNING) {
    int64_t delta = s_deadline_us - esp_timer_get_time();
    s_remaining = delta > 0 ? (uint32_t)((delta + 999999) / 1000000) : 0;
    s_state = HOME_POMO_PAUSED;
    s_deadline_us = 0;
    s_deadline_epoch = 0;
  } else {
    s_state = HOME_POMO_READY;
    s_remaining = duration_for(s_kind);
    s_deadline_us = 0;
    s_deadline_epoch = 0;
  }
  persist_pomodoro();
}

void home_dashboard_pomodoro_view(home_pomo_view_t *out) {
  if (!out)
    return;
  uint32_t remaining = s_remaining;
  if (s_state == HOME_POMO_RUNNING) {
    int64_t delta = s_deadline_us - esp_timer_get_time();
    remaining = delta > 0 ? (uint32_t)((delta + 999999) / 1000000) : 0;
  }
  *out = (home_pomo_view_t){
      .kind = s_kind,
      .state = s_state,
      .remaining_seconds = remaining,
      .total_seconds = duration_for(s_kind),
      .completed_focuses = s_completed,
      .just_completed = s_just_completed,
  };
}

bool home_dashboard_tick(void) {
  home_pomo_view_t view;
  home_dashboard_pomodoro_view(&view);
  if (s_state == HOME_POMO_RUNNING && view.remaining_seconds == 0)
    prepare_next_interval();
  home_dashboard_pomodoro_view(&view);
  bool changed = view.remaining_seconds != s_last_second;
  s_last_second = view.remaining_seconds;
  return changed;
}
