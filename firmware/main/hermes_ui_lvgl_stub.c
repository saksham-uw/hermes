/*
 * Optional LVGL UI for Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Enable once Waveshare's AXS15231B QSPI panel + touch drivers are added to
 * EXTRA_COMPONENT_DIRS, then set CONFIG_HERMES_UI_CONSOLE=n and link this file
 * from main/CMakeLists.txt instead of (or in addition to) the console UI.
 *
 * Screens: Status | Approval (Approve/Deny) | Prompt presets
 */
#include "esp_log.h"

static const char *TAG = "hermes_ui_lvgl";

void hermes_ui_lvgl_start(void) {
  ESP_LOGW(TAG, "LVGL UI stub — add Waveshare BSP and implement screens here");
}
