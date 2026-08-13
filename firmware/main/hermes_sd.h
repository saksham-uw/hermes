#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define HERMES_SD_MOUNT_POINT "/sdcard"

esp_err_t hermes_sd_mount(void);
bool hermes_sd_ready(void);
