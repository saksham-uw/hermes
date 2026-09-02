#pragma once

#include "hermes_gfx.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  HERMES_AVATAR_STATE_IDLE = 0,
  HERMES_AVATAR_STATE_REST,
  HERMES_AVATAR_STATE_BUSY,
  HERMES_AVATAR_STATE_THINKING,
  HERMES_AVATAR_STATE_COUNT,
} hermes_avatar_state_t;

typedef struct {
  uint32_t last_activity_ms;
  uint32_t phase_started_ms;
  uint32_t next_blink_ms;
  uint32_t blink_started_ms;
  uint32_t last_frame_ms;
  uint32_t random_state;
  uint8_t action;
  uint8_t step;
  uint8_t from_expression;
  uint8_t state;
  uint8_t requested_state;
  bool transition_phase;
  bool state_change_pending;
} hermes_avatar_t;

void hermes_avatar_init(hermes_avatar_t *avatar, uint32_t now_ms);
void hermes_avatar_note_activity(hermes_avatar_t *avatar, uint32_t now_ms);
bool hermes_avatar_tick(hermes_avatar_t *avatar, uint32_t now_ms);
bool hermes_avatar_play(hermes_avatar_t *avatar, const char *animation_key,
                        uint32_t now_ms);
bool hermes_avatar_set_state(hermes_avatar_t *avatar,
                             hermes_avatar_state_t state, uint32_t now_ms);
void hermes_avatar_draw(hermes_gfx_t *gfx, const hermes_avatar_t *avatar,
                        int cx, int cy, uint32_t now_ms);
const char *hermes_avatar_action_name(const hermes_avatar_t *avatar);
const char *hermes_avatar_state_name(const hermes_avatar_t *avatar);
