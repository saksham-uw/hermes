idf_component_register(
  SRCS
    "main.c"
    "wifi_connect.c"
    "hermes_ui.c"
    "hermes_actions.c"
    "home_dashboard.c"
    "home_data.c"
    "home_clock_assets.c"
    "hermes_avatar.c"
    "hermes_avatar_assets.c"
    "hermes_i2c.c"
    "hermes_touch.c"
    "hermes_gfx.c"
    "hermes_settings.c"
    "display_axs15231b.c"
  INCLUDE_DIRS "."
  REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_timer
    driver
    esp_adc
    esp_http_client
    json
    esp-tls
)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/hermes_actions_config.h")
  file(WRITE "${CMAKE_CURRENT_LIST_DIR}/hermes_actions_config.h"
"#pragma once
#define HERMES_ACTIONS_URL \"\"
#define HERMES_ACTIONS_DEVICE_TOKEN \"\"
")
endif()
