#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
  HOME_POMO_FOCUS = 0,
  HOME_POMO_SHORT_BREAK,
  HOME_POMO_LONG_BREAK,
} home_pomo_kind_t;

typedef enum {
  HOME_POMO_READY = 0,
  HOME_POMO_RUNNING,
  HOME_POMO_PAUSED,
} home_pomo_state_t;

typedef struct {
  home_pomo_kind_t kind;
  home_pomo_state_t state;
  uint32_t remaining_seconds;
  uint32_t total_seconds;
  uint8_t completed_focuses;
  bool just_completed;
} home_pomo_view_t;

esp_err_t home_dashboard_init(void);
esp_err_t home_dashboard_start_network_time(void);
bool home_dashboard_local_time(struct tm *out);
void home_dashboard_pomodoro_boot(void);
void home_dashboard_pomodoro_view(home_pomo_view_t *out);
bool home_dashboard_tick(void);
