#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t hermes_audio_init(void);
bool hermes_audio_ok(void);
esp_err_t hermes_audio_start(void);
/* Stop capture, upload PCM to the bridge for STT. Safe if already stopped at cap. */
esp_err_t hermes_audio_stop_and_upload(void);
bool hermes_audio_recording(void);
/* PCM captured and ready (recording ended or hit the time cap). */
bool hermes_audio_clip_ready(void);
