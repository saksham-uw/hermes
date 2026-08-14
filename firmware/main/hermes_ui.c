#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "cJSON.h"
#include "board_pins.h"
#include "hermes_ui.h"
#include "hermes_mqtt.h"
#include "hermes_i2c.h"
#include "hermes_touch.h"
#include "hermes_gfx.h"
#include "display_axs15231b.h"
#include "wifi_connect.h"
#include "hermes_sd.h"
#include "hermes_settings.h"
#include "hermes_audio.h"

static const char *TAG = "hermes_ui";

#define EDGE_PX 24
#define SWIPE_MIN 40
#define TAP_SLOP 20
#define BTN_LONG_MS 900

typedef enum {
  SCR_MENU = 0,
  SCR_CODEX,
  SCR_CURSOR,
  SCR_STATUS,
  SCR_SETTINGS,
  SCR_WIFI,
} screen_t;

#define LOG_CAP 48
#define LOG_LINE_LEN 120
#define CHAT_CAP 8
#define PANE_RIGHT 52
#define PANE_LEFT 148
#define DRAFT_CAP 1024

typedef struct {
  char id[48];
  char title[40];
  char preview[80];
  bool live;
} chat_item_t;

typedef struct {
  chat_item_t chats[CHAT_CAP];
  int chat_n;
  char state[24];
  char cwd[40];
  char quota[16];
  char model[28];
  char reasoning[16];
  char last[2304];
  char approval_id[64];
  char approval_title[64];
  char approval_detail[120];
  bool auto_approve;
} agent_view_t;

static hermes_gfx_t s_gfx;
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty = true;
static bool s_ready_ui;

static screen_t s_screen = SCR_MENU;
static int s_menu_idx = 0;
static int s_settings_idx = 0;
static int s_wifi_idx = 0;
static int s_chat_idx = 0;
static int s_agent_depth = 0; /* 0=list, 1=chat */
static int s_approve_idx = 0; /* 0=yes 1=always 2=no */
static int s_msg_scroll = 0;
static char s_draft[DRAFT_CAP];
static bool s_stt_busy;
static bool s_right_hold;
static int s_bksp_ticks;
static hermes_theme_t s_theme = THEME_NIGHT;
static uint8_t s_brightness = 85;

static agent_view_t s_codex;
static agent_view_t s_cursor_v;
static char s_status[160] = "boot";
static bool s_inet_cached;
static TickType_t s_inet_checked_at;

static bool internet_cached(void) {
  TickType_t now = xTaskGetTickCount();
  if ((now - s_inet_checked_at) > pdMS_TO_TICKS(5000) || s_inet_checked_at == 0) {
    s_inet_cached = hermes_wifi_has_internet();
    s_inet_checked_at = now;
  }
  return s_inet_cached;
}

static const char *MENU[] = {"codex", "cursor", "status", "settings", "wifi", "switch off"};
static const int MENU_N = 6;
static const int SETT_N = 4;
static const int APPROVE_N = 3;

static agent_view_t *agent_for_screen(screen_t scr) {
  return (scr == SCR_CURSOR) ? &s_cursor_v : &s_codex;
}

static const char *cli_label(const agent_view_t *a) {
  if (a->approval_id[0] || strcmp(a->state, "waiting") == 0 ||
      strcmp(a->state, "waiting_approval") == 0)
    return "waiting";
  if (strcmp(a->state, "running") == 0 || strcmp(a->state, "thinking") == 0)
    return "running";
  return "idle";
}

static bool cli_waiting(const agent_view_t *a) {
  return strcmp(cli_label(a), "waiting") == 0;
}

static bool cli_idle(const agent_view_t *a) {
  return strcmp(cli_label(a), "idle") == 0;
}

static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ok;

static void mark_dirty(void) { s_dirty = true; }

/* ——— battery (Waveshare divider on GPIO4) ——— */
static int battery_percent(void) {
  if (!s_adc_ok) return -1;
  int raw = 0;
  if (adc_oneshot_read(s_adc, ADC_CHANNEL_3, &raw) != ESP_OK) return -1;
  /* Uncalibrated estimate with 1:3 divider on GPIO4. */
  int pack_mv = (raw * 3100 / 4095) * 3;
  if (pack_mv < 2500) return -1;
  int pct = (pack_mv - 3300) * 100 / 900;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

static void adc_init(void) {
  adc_oneshot_unit_init_cfg_t cfg = {.unit_id = ADC_UNIT_1};
  if (adc_oneshot_new_unit(&cfg, &s_adc) != ESP_OK) return;
  adc_oneshot_chan_cfg_t ch = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12};
  if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL_3, &ch) == ESP_OK) s_adc_ok = true;
}

/* ——— power off ——— */
static void do_switch_off(void) {
  static const char *off_lines[] = {
      "sealing the courier bag",
      "dimming the caduceus",
      "parking the agents",
      "see you after lunch",
  };
  hermes_gfx_clear(&s_gfx, COL_BG);
  int y = hermes_gfx_draw_hermes_logo(&s_gfx, 16, 16, COL_FOCUS, 1);
  hermes_gfx_fill_rect(&s_gfx, 16, y + 10, GFX_W - 32, 10, COL_LINE);
  hermes_gfx_fill_rect(&s_gfx, 16, y + 10, GFX_W - 32, 10, COL_FOCUS);
  hermes_gfx_text(&s_gfx, 16, y + 28, off_lines[esp_log_timestamp() % 4], COL_DIM, 1);
  hermes_gfx_flush(&s_gfx);
  vTaskDelay(pdMS_TO_TICKS(1600));
  hermes_display_set_backlight(false);
  hermes_power_hold_release();
  /* Deep sleep until PWR (GPIO16) low */
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 0);
  esp_deep_sleep_start();
}

/* ——— boot screen (3s) ——— */
static void boot_screen(bool shutting_down) {
  static const char *boot_sub[] = {
      "warming the radio",
      "untangling mqtt threads",
      "polishing the remote",
      "asking codex to stretch",
      "calibrating pocket gravity",
  };
  static const char *off_sub[] = {
      "folding the menus away",
      "cutting the power rail",
      "hermes signing off",
      "courier going dark",
  };
  const char **subs = shutting_down ? off_sub : boot_sub;
  const int nsub = shutting_down ? 4 : 5;

  const int frames = 30; /* ~3s at 100ms */
  for (int f = 0; f <= frames; f++) {
    hermes_gfx_clear(&s_gfx, COL_BG);
    int y = hermes_gfx_draw_hermes_logo(&s_gfx, 16, 12, COL_FOCUS, 1);
    int bar_y = y + 12;
    int bar_w = GFX_W - 32;
    hermes_gfx_rect(&s_gfx, 16, bar_y, bar_w, 12, COL_LINE);
    int fill = (bar_w - 4) * f / frames;
    hermes_gfx_fill_rect(&s_gfx, 18, bar_y + 2, fill, 8, COL_FOCUS);
    const char *sub = subs[(f / 6) % nsub];
    hermes_gfx_text(&s_gfx, 16, bar_y + 20, sub, COL_DIM, 1);
    hermes_gfx_flush(&s_gfx);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (shutting_down) {
    hermes_display_set_backlight(false);
    hermes_power_hold_release();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 0);
    esp_deep_sleep_start();
  }
}

/* ——— screens ——— */
static void draw_menu(void) {
  hermes_gfx_text(&s_gfx, 12, 4, "HERMES", COL_DIM, 1);
  hermes_gfx_hline(&s_gfx, 12, 14, GFX_W - 24, COL_LINE);
  int y = 20;
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == s_menu_idx);
    uint16_t col = sel ? COL_FOCUS : COL_TEXT;
    char line[40];
    snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", MENU[i]);
    hermes_gfx_text(&s_gfx, 16, y, line, col, 2);
    y += 24;
  }
}

static void draw_bars(int x, int y, int w, int h, int pct, uint16_t fg) {
  hermes_gfx_rect(&s_gfx, x, y, w, h, COL_LINE);
  int fill = (w - 2) * pct / 100;
  if (fill > 0) hermes_gfx_fill_rect(&s_gfx, x + 1, y + 1, fill, h - 2, fg);
}

static void publish_approval(int choice) {
  /* 0=yes 1=always 2=no */
  agent_view_t *a = agent_for_screen(s_screen);
  if (!a || !a->approval_id[0]) return;
  char json[192];
  if (choice == 2) {
    snprintf(json, sizeof(json), "{\"type\":\"deny\",\"id\":\"%s\"}", a->approval_id);
    strncpy(a->state, "idle", sizeof(a->state) - 1);
  } else {
    snprintf(json, sizeof(json), "{\"type\":\"approve\",\"id\":\"%s\",\"always\":%s}",
             a->approval_id, choice == 1 ? "true" : "false");
    strncpy(a->state, "running", sizeof(a->state) - 1);
  }
  hermes_mqtt_publish_up(json);
  a->approval_id[0] = 0;
  a->approval_title[0] = 0;
  a->approval_detail[0] = 0;
}

static void draw_tick(int x, int y, uint16_t col) {
  /* simple check mark */
  hermes_gfx_fill_rect(&s_gfx, x + 2, y + 10, 3, 3, col);
  hermes_gfx_fill_rect(&s_gfx, x + 5, y + 12, 3, 3, col);
  hermes_gfx_fill_rect(&s_gfx, x + 8, y + 8, 3, 3, col);
  hermes_gfx_fill_rect(&s_gfx, x + 11, y + 4, 3, 3, col);
  hermes_gfx_fill_rect(&s_gfx, x + 14, y + 1, 3, 3, col);
}

static void draw_cross(int x, int y, uint16_t col) {
  for (int i = 0; i < 14; i++) {
    hermes_gfx_fill_rect(&s_gfx, x + 2 + i, y + 2 + i, 2, 2, col);
    hermes_gfx_fill_rect(&s_gfx, x + 16 - i, y + 2 + i, 2, 2, col);
  }
}

static void draw_mic(int x, int y, uint16_t col) {
  hermes_gfx_fill_rect(&s_gfx, x + 6, y, 8, 12, col);
  hermes_gfx_fill_rect(&s_gfx, x + 4, y + 2, 12, 8, col);
  hermes_gfx_rect(&s_gfx, x + 2, y + 4, 16, 12, col);
  hermes_gfx_fill_rect(&s_gfx, x + 9, y + 16, 2, 4, col);
  hermes_gfx_fill_rect(&s_gfx, x + 5, y + 19, 10, 2, col);
}

static void draw_bksp(int x, int y, uint16_t col) {
  for (int i = 0; i < 8; i++) {
    hermes_gfx_fill_rect(&s_gfx, x + 8 - i, y + 4 + i, 2, 2, col);
    hermes_gfx_fill_rect(&s_gfx, x + 8 - i, y + 18 - i, 2, 2, col);
  }
  hermes_gfx_fill_rect(&s_gfx, x + 8, y + 8, 12, 8, col);
  hermes_gfx_fill_rect(&s_gfx, x + 12, y + 10, 2, 4, COL_BG);
  hermes_gfx_fill_rect(&s_gfx, x + 16, y + 10, 2, 4, COL_BG);
}

static void json_escape(char *out, size_t n, const char *in) {
  size_t o = 0;
  if (!out || n == 0) return;
  for (; *in && o + 2 < n; in++) {
    if (*in == '\\' || *in == '"') {
      if (o + 3 >= n) break;
      out[o++] = '\\';
      out[o++] = *in;
    } else if (*in == '\n') {
      if (o + 3 >= n) break;
      out[o++] = '\\';
      out[o++] = 'n';
    } else if ((unsigned char)*in >= 32) {
      out[o++] = *in;
    }
  }
  out[o] = 0;
}

static void send_draft(void) {
  if (!s_draft[0] || !hermes_mqtt_is_connected()) return;
  static char esc[DRAFT_CAP * 2];
  static char json[DRAFT_CAP * 2 + 64];
  json_escape(esc, sizeof(esc), s_draft);
  snprintf(json, sizeof(json), "{\"type\":\"prompt\",\"agent\":\"codex\",\"text\":\"%s\"}", esc);
  hermes_mqtt_publish_up(json);
  s_draft[0] = 0;
  s_stt_busy = false;
  mark_dirty();
}

static void draft_del(int n) {
  int len = (int)strlen(s_draft);
  if (n > len) n = len;
  if (n <= 0) return;
  s_draft[len - n] = 0;
  mark_dirty();
}

static void draw_agent(const char *title, agent_view_t *a) {
  hermes_gfx_text(&s_gfx, 12, 4, title, COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, 12, 14, GFX_W - 24, COL_LINE);

  if (s_agent_depth == 0) {
    if (a->chat_n == 0) {
      hermes_gfx_text(&s_gfx, 16, 40, "> (no chats yet)", COL_DIM, 1);
      hermes_gfx_text(&s_gfx, 16, 56, "  waiting for bridge...", COL_DIM, 1);
    } else {
      int y = 22;
      for (int i = 0; i < a->chat_n; i++) {
        bool sel = (i == s_chat_idx);
        char line[48];
        snprintf(line, sizeof(line), "%s %s%s", sel ? ">" : " ", a->chats[i].title,
                 a->chats[i].live ? " *" : "");
        hermes_gfx_text(&s_gfx, 12, y, line, sel ? COL_FOCUS : COL_TEXT, 1);
        hermes_gfx_text(&s_gfx, 24, y + 12, a->chats[i].preview[0] ? a->chats[i].preview : "-",
                        COL_DIM, 1);
        y += 28;
        if (y > GFX_H - 30) break;
      }
    }
    hermes_gfx_text(&s_gfx, 12, GFX_H - 14, "tap=open chat   PWR=menu", COL_DIM, 1);
    return;
  }

  /* 3-pane: left meta | middle last+draft | right approve or mic */
  const char *cli = cli_label(a);
  bool waiting = cli_waiting(a);
  bool idle = cli_idle(a) && s_screen == SCR_CODEX;
  int top = 18;
  int left_w = PANE_LEFT;
  int right_w = (waiting || idle) ? PANE_RIGHT : 0;
  int mid_x = left_w + 8;
  int mid_w = GFX_W - mid_x - right_w - 8;

  hermes_gfx_vline(&s_gfx, left_w, top, GFX_H - top - 4, COL_LINE);
  if (right_w) hermes_gfx_vline(&s_gfx, GFX_W - right_w, top, GFX_H - top - 4, COL_LINE);

  int y = top + 2;
  hermes_gfx_text(&s_gfx, 8, y, "DIR", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 8, y + 10, a->cwd[0] ? a->cwd : "-", COL_TEXT, 1);
  y += 28;
  hermes_gfx_text(&s_gfx, 8, y, "WEEK", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 8, y + 10, a->quota[0] ? a->quota : "-", COL_FOCUS, 1);
  y += 28;
  hermes_gfx_text(&s_gfx, 8, y, "MODEL", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 8, y + 10, a->model[0] ? a->model : "-", COL_TEXT, 1);
  y += 28;
  hermes_gfx_text(&s_gfx, 8, y, "REASON", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 8, y + 10, a->reasoning[0] ? a->reasoning : "-", COL_TEXT, 1);
  y += 28;
  hermes_gfx_text(&s_gfx, 8, y, "CLI", COL_DIM, 1);
  uint16_t stcol = waiting ? COL_WARN : (strcmp(cli, "running") == 0 ? COL_FOCUS : COL_OK);
  hermes_gfx_text(&s_gfx, 8, y + 10, cli, stcol, 1);

  int mid_y = top + 2;
  int mid_h = GFX_H - mid_y - 4;
  bool rec = hermes_audio_recording();
  bool show_draft = idle && (s_draft[0] || s_stt_busy || rec);
  int draft_h = 0;
  if (show_draft) {
    draft_h = 44;
    if (draft_h > mid_h / 2) draft_h = mid_h / 2;
    mid_h -= draft_h;
  }
  const char *body = a->last[0] ? a->last : "";
  if (waiting && a->approval_title[0]) {
    hermes_gfx_text_wrap_clip(&s_gfx, mid_x, mid_y, mid_w, 16, 0, a->approval_title, COL_WARN, 1);
    mid_y += 18;
    mid_h -= 18;
    if (!body[0]) body = a->approval_detail;
  }
  if (strcmp(cli, "running") == 0 && !waiting) {
    hermes_gfx_text(&s_gfx, mid_x, top + 40, "(running)", COL_DIM, 1);
  } else if (body[0]) {
    int vis = mid_h / 8;
    if (vis < 1) vis = 1;
    int total = hermes_gfx_text_wrap_clip(NULL, mid_x, mid_y, mid_w, 0, 0, body, COL_TEXT, 1);
    int max_scroll = total > vis ? total - vis : 0;
    if (s_msg_scroll > max_scroll) s_msg_scroll = max_scroll;
    if (s_msg_scroll < 0) s_msg_scroll = 0;
    hermes_gfx_text_wrap_clip(&s_gfx, mid_x, mid_y, mid_w, mid_h, s_msg_scroll, body, COL_TEXT, 1);
    if (max_scroll > 0) {
      hermes_gfx_text(&s_gfx, mid_x + mid_w - 18, mid_y + mid_h - 10, "v^", COL_DIM, 1);
    }
  } else if (!show_draft) {
    hermes_gfx_text(&s_gfx, mid_x, top + 40, "(no last message)", COL_DIM, 1);
  }

  if (show_draft) {
    int dy = top + 2 + (GFX_H - top - 4 - draft_h);
    hermes_gfx_hline(&s_gfx, mid_x, dy, mid_w, COL_LINE);
    const char *d = rec ? "(listening)" : (s_stt_busy ? "(transcribing)" : s_draft);
    uint16_t dcol = rec ? COL_WARN : (s_stt_busy ? COL_DIM : COL_FOCUS);
    hermes_gfx_text_wrap_clip(&s_gfx, mid_x, dy + 3, mid_w, draft_h - 4, 0, d, dcol, 1);
  }

  if (waiting) {
    int slot_h = (GFX_H - top) / 3;
    int rx = GFX_W - right_w + (right_w - 20) / 2;
    for (int i = 0; i < 3; i++) {
      int iy = top + i * slot_h + (slot_h - 18) / 2;
      bool sel = (i == s_approve_idx);
      uint16_t col = sel ? COL_FOCUS : COL_TEXT;
      if (sel) hermes_gfx_rect(&s_gfx, GFX_W - right_w + 2, top + i * slot_h + 2, right_w - 6,
                               slot_h - 4, COL_FOCUS);
      if (i == 0) draw_tick(rx, iy, col);
      else if (i == 1) {
        draw_tick(rx - 6, iy, col);
        draw_tick(rx + 4, iy, col);
      } else {
        draw_cross(rx, iy, sel ? COL_BAD : COL_TEXT);
      }
    }
  } else if (idle) {
    int split = top + (GFX_H - top) / 2;
    int rx = GFX_W - right_w + (right_w - 20) / 2;
    uint16_t mcol = rec ? COL_WARN : COL_TEXT;
    if (rec) hermes_gfx_rect(&s_gfx, GFX_W - right_w + 2, top + 2, right_w - 6, split - top - 4, COL_WARN);
    draw_mic(rx, top + (split - top - 22) / 2, mcol);
    draw_bksp(rx, split + (GFX_H - split - 22) / 2, COL_TEXT);
  }
}

static void draw_status(void) {
  hermes_gfx_text(&s_gfx, 12, 6, "// STATUS", COL_FOCUS, 1);
  hermes_gfx_hline(&s_gfx, 12, 18, GFX_W - 24, COL_LINE);

  int bat = battery_percent();
  int rssi = hermes_wifi_rssi();
  int sig = 0;
  if (rssi != 0) {
    /* -30 excellent … -90 bad */
    sig = (rssi + 90) * 100 / 60;
    if (sig < 0) sig = 0;
    if (sig > 100) sig = 100;
  }

  char line[64];
  hermes_gfx_text(&s_gfx, 12, 28, "BAT", COL_DIM, 1);
  if (bat >= 0) {
    snprintf(line, sizeof(line), "%3d%%", bat);
    hermes_gfx_text(&s_gfx, 50, 28, line, COL_TEXT, 1);
    draw_bars(100, 28, 120, 10, bat, bat < 20 ? COL_BAD : COL_OK);
  } else {
    hermes_gfx_text(&s_gfx, 50, 28, "usb?", COL_DIM, 1);
  }

  hermes_gfx_text(&s_gfx, 12, 48, "NET", COL_DIM, 1);
  if (hermes_wifi_is_connected()) {
    snprintf(line, sizeof(line), "%ddBm", rssi);
    hermes_gfx_text(&s_gfx, 50, 48, line, COL_TEXT, 1);
    draw_bars(100, 48, 120, 10, sig, COL_FOCUS);
  } else {
    hermes_gfx_text(&s_gfx, 50, 48, "DOWN", COL_BAD, 1);
  }

  hermes_gfx_text(&s_gfx, 12, 70, "CDX", COL_DIM, 1);
  snprintf(line, sizeof(line), "[%s]", s_codex.state[0] ? s_codex.state : "idle");
  hermes_gfx_text(&s_gfx, 50, 70, line, COL_FOCUS, 1);

  hermes_gfx_text(&s_gfx, 12, 90, "CUR", COL_DIM, 1);
  snprintf(line, sizeof(line), "[%s]", s_cursor_v.state[0] ? s_cursor_v.state : "unknown");
  hermes_gfx_text(&s_gfx, 50, 90, line, COL_TEXT, 1);

  hermes_gfx_text(&s_gfx, 12, 110, "MQT", COL_DIM, 1);
  hermes_gfx_text(&s_gfx, 50, 110, hermes_mqtt_is_connected() ? "LINKED" : "offline",
                  hermes_mqtt_is_connected() ? COL_OK : COL_WARN, 1);

  hermes_gfx_text(&s_gfx, 12, 132, "LOG", COL_DIM, 1);
  hermes_gfx_text_wrap(&s_gfx, 50, 132, GFX_W - 60, s_status, COL_TEXT, 1);

  hermes_gfx_text(&s_gfx, 12, GFX_H - 14, "PWR short = menu", COL_DIM, 1);
}

static void draw_settings(void) {
  hermes_gfx_text(&s_gfx, 12, 8, "SETTINGS", COL_DIM, 1);
  hermes_gfx_hline(&s_gfx, 12, 20, GFX_W - 24, COL_LINE);

  char theme_line[40];
  snprintf(theme_line, sizeof(theme_line), "theme: %s", hermes_theme_name(s_theme));
  char bri_line[40];
  snprintf(bri_line, sizeof(bri_line), "brightness: %d%%", s_brightness);
  const char *rows[4];
  rows[0] = theme_line;
  rows[1] = "brightness +";
  rows[2] = "brightness -";
  rows[3] = "back";
  (void)bri_line;
  hermes_gfx_text(&s_gfx, 16, 26, bri_line, COL_DIM, 1);

  int y = 48;
  for (int i = 0; i < SETT_N; i++) {
    bool sel = (i == s_settings_idx);
    char line[48];
    snprintf(line, sizeof(line), "%s %s", sel ? ">" : " ", rows[i]);
    hermes_gfx_text(&s_gfx, 16, y, line, sel ? COL_FOCUS : COL_TEXT, 2);
    y += 26;
  }
}

static void draw_wifi(void) {
  hermes_gfx_text(&s_gfx, 12, 8, "WIFI", COL_DIM, 1);
  hermes_gfx_hline(&s_gfx, 12, 20, GFX_W - 24, COL_LINE);
  int y = 28;
  int active = hermes_wifi_active_index();
  for (int i = 0; i < hermes_wifi_count(); i++) {
    const hermes_wifi_net_t *n = hermes_wifi_net(i);
    const hermes_wifi_sight_t *s = hermes_wifi_sight(i);
    bool sel = (i == s_wifi_idx);
    bool on = (i == active) && hermes_wifi_is_connected();
    bool ok = s && s->visible;
    uint16_t col = !ok ? COL_DIM : (sel ? COL_FOCUS : COL_TEXT);

    char left[40];
    snprintf(left, sizeof(left), "%s%s %s", sel ? ">" : " ", on ? "*" : (ok ? " " : "x"), n->ssid);
    hermes_gfx_text(&s_gfx, 8, y, left, col, 1);
    hermes_gfx_text(&s_gfx, 20, y + 12, n->label, COL_DIM, 1);

    /* Right-aligned stats */
    char right[32];
    if (on) {
      int rssi = hermes_wifi_rssi();
      bool net = internet_cached();
      snprintf(right, sizeof(right), "%ddBm %s", rssi ? rssi : (s ? s->rssi : 0),
               net ? "UP" : "NO NET");
      hermes_gfx_text(&s_gfx, GFX_W - 8 - hermes_gfx_text_width(right, 1), y, right,
                      net ? COL_OK : COL_WARN, 1);
    } else if (ok) {
      snprintf(right, sizeof(right), "%ddBm ch%d", s->rssi, s->channel);
      hermes_gfx_text(&s_gfx, GFX_W - 8 - hermes_gfx_text_width(right, 1), y, right, COL_DIM, 1);
      const char *st = "in range";
      hermes_gfx_text(&s_gfx, GFX_W - 8 - hermes_gfx_text_width(st, 1), y + 12, st, COL_DIM, 1);
    } else {
      const char *st = "not found";
      hermes_gfx_text(&s_gfx, GFX_W - 8 - hermes_gfx_text_width(st, 1), y, st, COL_BAD, 1);
    }
    y += 40;
  }
  hermes_gfx_text(&s_gfx, 12, GFX_H - 14, "tap=switch if in range   PWR=menu", COL_DIM, 1);
}

static void activate_wifi(void) {
  int idx = s_wifi_idx;
  if (!hermes_wifi_is_selectable(idx)) {
    snprintf(s_status, sizeof(s_status), "%s not in range", hermes_wifi_net(idx)->ssid);
    mark_dirty();
    return;
  }
  hermes_gfx_clear(&s_gfx, COL_BG);
  hermes_gfx_text_centered(&s_gfx, 70, "checking link...", COL_WARN, 2);
  hermes_gfx_flush(&s_gfx);

  esp_err_t err = hermes_wifi_select(idx);
  if (err == ESP_OK) {
    snprintf(s_status, sizeof(s_status), "on %s", hermes_wifi_net(idx)->ssid);
  } else if (err == ESP_ERR_NOT_FOUND) {
    snprintf(s_status, sizeof(s_status), "not in range");
  } else {
    snprintf(s_status, sizeof(s_status), "kept prior wifi (no internet)");
  }
  hermes_wifi_refresh_scan();
  s_inet_checked_at = 0; /* force recheck after switch */
  mark_dirty();
}

static void render_lcd(void) {
  hermes_gfx_clear(&s_gfx, COL_BG);
  switch (s_screen) {
    case SCR_MENU: draw_menu(); break;
    case SCR_CODEX: draw_agent("CODEX", &s_codex); break;
    case SCR_CURSOR: draw_agent("CURSOR", &s_cursor_v); break;
    case SCR_STATUS: draw_status(); break;
    case SCR_SETTINGS: draw_settings(); break;
    case SCR_WIFI: draw_wifi(); break;
  }
  hermes_gfx_flush(&s_gfx);
}

static void menu_move(int delta, int *idx, int n) {
  if (n <= 0) return;
  *idx = (*idx + delta + n) % n;
  mark_dirty();
}

static void go_menu(void) {
  s_screen = SCR_MENU;
  s_agent_depth = 0;
  mark_dirty();
}

static void activate_menu(void) {
  switch (s_menu_idx) {
    case 0:
      s_screen = SCR_CODEX;
      s_agent_depth = 0;
      s_chat_idx = 0;
      hermes_mqtt_publish_up("{\"type\":\"refresh_chats\",\"agent\":\"codex\"}");
      mark_dirty();
      break;
    case 1:
      s_screen = SCR_CURSOR;
      s_agent_depth = 0;
      s_chat_idx = 0;
      mark_dirty();
      break;
    case 2: s_screen = SCR_STATUS; mark_dirty(); break;
    case 3: s_screen = SCR_SETTINGS; s_settings_idx = 0; mark_dirty(); break;
    case 4:
      s_screen = SCR_WIFI;
      s_wifi_idx = hermes_wifi_active_index();
      hermes_wifi_refresh_scan();
      mark_dirty();
      break;
    case 5: do_switch_off(); break;
  }
}

static void activate_settings(void) {
  if (s_settings_idx == 0) {
    s_theme = (hermes_theme_t)((s_theme + 1) % THEME_COUNT);
    hermes_theme_apply(s_theme);
    hermes_settings_save(s_theme, s_brightness);
    mark_dirty();
  } else if (s_settings_idx == 1) {
    if (s_brightness < 100) s_brightness = (uint8_t)(s_brightness + 15);
    if (s_brightness > 100) s_brightness = 100;
    hermes_display_set_brightness(s_brightness);
    hermes_settings_save(s_theme, s_brightness);
    mark_dirty();
  } else if (s_settings_idx == 2) {
    if (s_brightness > 15) s_brightness = (uint8_t)(s_brightness - 15);
    else s_brightness = 5;
    hermes_display_set_brightness(s_brightness);
    hermes_settings_save(s_theme, s_brightness);
    mark_dirty();
  } else {
    go_menu();
  }
}

static void activate_agent(void) {
  agent_view_t *a = agent_for_screen(s_screen);
  if (!a) return;
  if (s_agent_depth == 0) {
    if (a->chat_n == 0) {
      hermes_mqtt_publish_up("{\"type\":\"refresh_chats\",\"agent\":\"codex\"}");
      mark_dirty();
      return;
    }
    if (s_chat_idx < 0 || s_chat_idx >= a->chat_n) s_chat_idx = 0;
    if (a->chats[s_chat_idx].id[0] && s_screen == SCR_CODEX) {
      char json[128];
      snprintf(json, sizeof(json),
               "{\"type\":\"select_chat\",\"agent\":\"codex\",\"id\":\"%s\"}",
               a->chats[s_chat_idx].id);
      hermes_mqtt_publish_up(json);
      a->last[0] = 0;
    }
      s_agent_depth = 1;
      s_approve_idx = 0;
      s_msg_scroll = 0;
      s_draft[0] = 0;
      s_stt_busy = false;
      mark_dirty();
    return;
  }
  if (cli_waiting(a)) {
    publish_approval(s_approve_idx);
    mark_dirty();
  }
}

static void handle_tap(uint16_t x, uint16_t y) {
  if (s_screen == SCR_CODEX && s_agent_depth == 1 && cli_waiting(&s_codex) &&
      x >= GFX_W - PANE_RIGHT) {
    int top = 18;
    int slot_h = (GFX_H - top) / 3;
    int slot = ((int)y - top) / slot_h;
    if (slot < 0) slot = 0;
    if (slot > 2) slot = 2;
    s_approve_idx = slot;
    publish_approval(slot);
    mark_dirty();
    return;
  }
  if (s_screen == SCR_MENU) activate_menu();
  else if (s_screen == SCR_SETTINGS) activate_settings();
  else if (s_screen == SCR_WIFI) activate_wifi();
  else if (s_screen == SCR_CODEX || s_screen == SCR_CURSOR) activate_agent();
  else if (s_screen == SCR_STATUS) go_menu();
}

static void handle_gesture(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  if (s_right_hold) {
    s_right_hold = false;
    return;
  }
  int dx = (int)x1 - (int)x0;
  int dy = (int)y1 - (int)y0;
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;

  if (ady >= SWIPE_MIN && ady > adx) {
    int dir = (dy < 0) ? -1 : +1; /* swipe up = previous */
    if (s_screen == SCR_MENU) menu_move(dir, &s_menu_idx, MENU_N);
    else if (s_screen == SCR_SETTINGS) menu_move(dir, &s_settings_idx, SETT_N);
    else if (s_screen == SCR_WIFI) menu_move(dir, &s_wifi_idx, hermes_wifi_count());
    else if (s_screen == SCR_CODEX || s_screen == SCR_CURSOR) {
      agent_view_t *a = agent_for_screen(s_screen);
      if (s_agent_depth == 0) menu_move(dir, &s_chat_idx, a->chat_n > 0 ? a->chat_n : 1);
      else if (cli_waiting(a)) menu_move(dir, &s_approve_idx, APPROVE_N);
      else {
        /* swipe up = later lines */
        s_msg_scroll += (dir < 0) ? 2 : -2;
        if (s_msg_scroll < 0) s_msg_scroll = 0;
        mark_dirty();
      }
    }
    return;
  }
  if (adx < TAP_SLOP && ady < TAP_SLOP) handle_tap(x1, y1);
}

void hermes_ui_on_down_json(const char *json, int len) {
  if (!s_ready_ui) return;
  char *buf = calloc(1, (size_t)len + 1);
  if (!buf) return;
  memcpy(buf, json, (size_t)len);
  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) return;
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  if (cJSON_IsString(type)) {
    if (strcmp(type->valuestring, "status") == 0) {
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      const cJSON *summary = cJSON_GetObjectItem(root, "summary");
      const cJSON *state = cJSON_GetObjectItem(root, "state");
      agent_view_t *a = &s_codex;
      if (cJSON_IsString(agent) && strcmp(agent->valuestring, "cursor") == 0) a = &s_cursor_v;
      if (cJSON_IsString(summary)) strncpy(s_status, summary->valuestring, sizeof(s_status) - 1);
      if (cJSON_IsString(state)) strncpy(a->state, state->valuestring, sizeof(a->state) - 1);
    } else if (strcmp(type->valuestring, "log") == 0) {
      const cJSON *text = cJSON_GetObjectItem(root, "text");
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      agent_view_t *a = &s_codex;
      if (cJSON_IsString(agent) && strcmp(agent->valuestring, "cursor") == 0) a = &s_cursor_v;
      if (cJSON_IsString(text)) {
        strncpy(s_status, text->valuestring, sizeof(s_status) - 1);
        if (a == &s_cursor_v) strncpy(a->last, text->valuestring, sizeof(a->last) - 1);
      }
    } else if (strcmp(type->valuestring, "chats") == 0) {
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      const cJSON *chats = cJSON_GetObjectItem(root, "chats");
      agent_view_t *a = &s_codex;
      if (cJSON_IsString(agent) && strcmp(agent->valuestring, "cursor") == 0) a = &s_cursor_v;
      a->chat_n = 0;
      if (cJSON_IsArray(chats)) {
        const cJSON *item = NULL;
        int i = 0;
        cJSON_ArrayForEach(item, chats) {
          if (i >= CHAT_CAP) break;
          const cJSON *id = cJSON_GetObjectItem(item, "id");
          const cJSON *title = cJSON_GetObjectItem(item, "title");
          const cJSON *preview = cJSON_GetObjectItem(item, "preview");
          const cJSON *live = cJSON_GetObjectItem(item, "live");
          memset(&a->chats[i], 0, sizeof(a->chats[i]));
          if (cJSON_IsString(id)) strncpy(a->chats[i].id, id->valuestring, sizeof(a->chats[i].id) - 1);
          if (cJSON_IsString(title))
            strncpy(a->chats[i].title, title->valuestring, sizeof(a->chats[i].title) - 1);
          else
            strncpy(a->chats[i].title, "chat", sizeof(a->chats[i].title) - 1);
          if (cJSON_IsString(preview))
            strncpy(a->chats[i].preview, preview->valuestring, sizeof(a->chats[i].preview) - 1);
          a->chats[i].live = cJSON_IsTrue(live);
          i++;
        }
        a->chat_n = i;
        if (s_chat_idx >= a->chat_n) s_chat_idx = 0;
        ESP_LOGI(TAG, "chats updated n=%d", a->chat_n);
      }
    } else if (strcmp(type->valuestring, "chat_view") == 0) {
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      agent_view_t *a = &s_codex;
      if (cJSON_IsString(agent) && strcmp(agent->valuestring, "cursor") == 0) a = &s_cursor_v;
      const cJSON *cwd = cJSON_GetObjectItem(root, "cwd");
      const cJSON *quota = cJSON_GetObjectItem(root, "quotaLeft");
      const cJSON *model = cJSON_GetObjectItem(root, "model");
      const cJSON *reason = cJSON_GetObjectItem(root, "reasoning");
      const cJSON *cli = cJSON_GetObjectItem(root, "cli");
      const cJSON *last = cJSON_GetObjectItem(root, "last");
      const cJSON *apid = cJSON_GetObjectItem(root, "approvalId");
      const cJSON *apt = cJSON_GetObjectItem(root, "approvalTitle");
      if (cJSON_IsString(cwd)) strncpy(a->cwd, cwd->valuestring, sizeof(a->cwd) - 1);
      if (cJSON_IsString(quota)) strncpy(a->quota, quota->valuestring, sizeof(a->quota) - 1);
      if (cJSON_IsString(model)) {
        strncpy(a->model, model->valuestring, sizeof(a->model) - 1);
        a->model[sizeof(a->model) - 1] = 0;
      }
      if (cJSON_IsString(reason)) {
        strncpy(a->reasoning, reason->valuestring, sizeof(a->reasoning) - 1);
        a->reasoning[sizeof(a->reasoning) - 1] = 0;
      }
      if (cJSON_IsString(cli)) strncpy(a->state, cli->valuestring, sizeof(a->state) - 1);
      if (cJSON_IsString(last)) {
        if (strncmp(a->last, last->valuestring, sizeof(a->last) - 1) != 0) s_msg_scroll = 0;
        strncpy(a->last, last->valuestring, sizeof(a->last) - 1);
        a->last[sizeof(a->last) - 1] = 0;
      }
      if (cJSON_IsString(apid) && apid->valuestring[0]) {
        strncpy(a->approval_id, apid->valuestring, sizeof(a->approval_id) - 1);
        s_approve_idx = 0;
      } else {
        a->approval_id[0] = 0;
      }
      if (cJSON_IsString(apt))
        strncpy(a->approval_title, apt->valuestring, sizeof(a->approval_title) - 1);
      else
        a->approval_title[0] = 0;
      ESP_LOGI(TAG, "chat_view cli=%s", a->state);
    } else if (strcmp(type->valuestring, "transcript") == 0) {
      const cJSON *text = cJSON_GetObjectItem(root, "text");
      s_stt_busy = false;
      if (cJSON_IsString(text) && text->valuestring[0]) {
        size_t used = strlen(s_draft);
        const char *add = text->valuestring;
        if (used && s_draft[used - 1] != ' ' && s_draft[used - 1] != '\n') {
          if (used + 1 < sizeof(s_draft) - 1) {
            s_draft[used++] = ' ';
            s_draft[used] = 0;
          }
        }
        strncat(s_draft, add, sizeof(s_draft) - 1 - strlen(s_draft));
      }
    } else if (strcmp(type->valuestring, "transcript_error") == 0) {
      s_stt_busy = false;
      const cJSON *m = cJSON_GetObjectItem(root, "message");
      if (cJSON_IsString(m)) strncpy(s_status, m->valuestring, sizeof(s_status) - 1);
    } else if (strcmp(type->valuestring, "approval") == 0) {
      const cJSON *id = cJSON_GetObjectItem(root, "id");
      const cJSON *agent = cJSON_GetObjectItem(root, "agent");
      const cJSON *title = cJSON_GetObjectItem(root, "title");
      const cJSON *detail = cJSON_GetObjectItem(root, "detail");
      agent_view_t *a = &s_codex;
      if (cJSON_IsString(agent) && strcmp(agent->valuestring, "cursor") == 0) a = &s_cursor_v;
      if (cJSON_IsString(id)) strncpy(a->approval_id, id->valuestring, sizeof(a->approval_id) - 1);
      if (cJSON_IsString(title))
        strncpy(a->approval_title, title->valuestring, sizeof(a->approval_title) - 1);
      if (cJSON_IsString(detail))
        strncpy(a->approval_detail, detail->valuestring, sizeof(a->approval_detail) - 1);
      strncpy(a->state, "waiting", sizeof(a->state) - 1);
      s_approve_idx = 0;
    } else if (strcmp(type->valuestring, "cursor") == 0) {
      const cJSON *summary = cJSON_GetObjectItem(root, "summary");
      if (cJSON_IsString(summary)) {
        strncpy(s_cursor_v.state, summary->valuestring, sizeof(s_cursor_v.state) - 1);
        strncpy(s_cursor_v.last, summary->valuestring, sizeof(s_cursor_v.last) - 1);
      }
    }
  }
  cJSON_Delete(root);
  mark_dirty();
  if (s_lock) xSemaphoreGive(s_lock);
}

typedef struct {
  bool prev;
  TickType_t down_at;
} btn_t;

static void ui_task(void *arg) {
  (void)arg;
  bool touch_down = false;
  uint16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  btn_t boot = {.prev = true};
  btn_t pwr = {.prev = true};

  gpio_config_t io = {
      .pin_bit_mask = (1ULL << HERMES_BOOT_PIN) | (1ULL << HERMES_PWR_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io);

  TickType_t last_status = 0;
  int mic_hold = 0;

  while (1) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty || (s_screen == SCR_STATUS && (xTaskGetTickCount() - last_status) > pdMS_TO_TICKS(1000))) {
      s_dirty = false;
      last_status = xTaskGetTickCount();
      render_lcd();
    }
    if (s_lock) xSemaphoreGive(s_lock);

    bool pressed = false;
    uint16_t tx = 0, ty = 0;
    hermes_touch_read(&pressed, &tx, &ty);
    if (pressed) {
      if (!touch_down) {
        touch_down = true;
        x0 = x1 = tx;
        y0 = y1 = ty;
        s_bksp_ticks = 0;
      } else {
        x1 = tx;
        y1 = ty;
      }
      if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
      if (s_screen == SCR_CODEX && s_agent_depth == 1 && cli_idle(&s_codex) &&
          tx >= GFX_W - PANE_RIGHT) {
        s_right_hold = true;
        int split = 18 + (GFX_H - 18) / 2;
        if (ty < split) {
          s_bksp_ticks = 0;
          mic_hold++;
          if (mic_hold == 6 && !hermes_audio_recording() && !s_stt_busy && hermes_audio_ok()) {
            hermes_audio_start();
            mark_dirty();
          }
        } else {
          mic_hold = 0;
          s_bksp_ticks++;
          int every = 8;
          if (s_bksp_ticks > 30) every = 3;
          if (s_bksp_ticks > 60) every = 1;
          if (s_bksp_ticks % every == 0) draft_del(1);
        }
      }
      if (s_lock) xSemaphoreGive(s_lock);
    } else if (touch_down) {
      touch_down = false;
      mic_hold = 0;
      if (hermes_audio_recording()) {
        hermes_audio_stop_and_upload();
        s_stt_busy = true;
        mark_dirty();
      }
      if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
      handle_gesture(x0, y0, x1, y1);
      if (s_lock) xSemaphoreGive(s_lock);
    }

    /* BOOT: in idle chat with draft = send; else back / menu */
    bool boot_lvl = gpio_get_level(HERMES_BOOT_PIN);
    if (boot.prev && !boot_lvl) boot.down_at = xTaskGetTickCount();
    else if (!boot.prev && boot_lvl) {
      TickType_t held = xTaskGetTickCount() - boot.down_at;
      if (held < pdMS_TO_TICKS(BTN_LONG_MS)) {
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_screen == SCR_CODEX && s_agent_depth == 1 && cli_idle(&s_codex) && s_draft[0]) {
          send_draft();
        } else if (s_screen == SCR_MENU) menu_move(+1, &s_menu_idx, MENU_N);
        else go_menu();
        if (s_lock) xSemaphoreGive(s_lock);
      }
    }
    boot.prev = boot_lvl;

    /* PWR: short = back to menu; long = switch off (boot-off screen) */
    bool pwr_lvl = gpio_get_level(HERMES_PWR_PIN);
    if (pwr.prev && !pwr_lvl) pwr.down_at = xTaskGetTickCount();
    else if (!pwr.prev && pwr_lvl) {
      TickType_t held = xTaskGetTickCount() - pwr.down_at;
      if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
      if (held >= pdMS_TO_TICKS(BTN_LONG_MS)) {
        boot_screen(true); /* never returns */
      } else if ((s_screen == SCR_CODEX || s_screen == SCR_CURSOR) && s_agent_depth > 0) {
        s_agent_depth = 0;
        mark_dirty();
      } else {
        go_menu();
      }
      if (s_lock) xSemaphoreGive(s_lock);
    }
    pwr.prev = pwr_lvl;

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

esp_err_t hermes_ui_start(void) {
  s_lock = xSemaphoreCreateMutex();

  if (hermes_sd_mount() == ESP_OK) {
    hermes_settings_load(&s_theme, &s_brightness);
  }
  hermes_theme_apply(s_theme);

  ESP_ERROR_CHECK(hermes_i2c_init());
  hermes_power_hold_enable();
  ESP_ERROR_CHECK(hermes_display_init());
  hermes_display_set_brightness(s_brightness);
  if (hermes_touch_init() != ESP_OK) ESP_LOGW(TAG, "touch failed");
  if (hermes_audio_init() != ESP_OK) ESP_LOGW(TAG, "audio/mic failed");
  if (!hermes_gfx_init(&s_gfx)) return ESP_ERR_NO_MEM;
  adc_init();

  boot_screen(false);

  s_screen = SCR_MENU;
  s_menu_idx = 0;
  s_ready_ui = true;
  render_lcd();
  xTaskCreate(ui_task, "hermes_ui", 8192, NULL, 5, NULL);
  ESP_LOGI(TAG, "design_doc UI ready");
  return ESP_OK;
}
