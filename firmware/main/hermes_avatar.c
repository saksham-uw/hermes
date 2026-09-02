#include "hermes_avatar.h"

#include "esp_log.h"
#include "hermes_avatar_assets.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/*
 * Visual geometry and semantic timelines are compiled from the official
 * @bible-strong/avatar-core SDK. This file only provides the ESP32 clock,
 * lifecycle policy, RGB565 compositor and decoder for those generated assets.
 */

#define FRAME_INTERVAL_MS 40U

static const char *TAG = "hermes_avatar";

enum {
  ACTION_WAKING = 1,
  ACTION_IDLE = 2,
};

typedef struct {
  const uint8_t *cursor;
  const uint8_t *end;
  uint16_t remaining;
  uint16_t value;
  bool run;
} frame_decoder_t;

static bool due(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

static uint32_t random_next(hermes_avatar_t *avatar) {
  avatar->random_state = avatar->random_state * 1664525U + 1013904223U;
  return avatar->random_state;
}

static const hermes_avatar_asset_animation_t *animation_at(uint8_t action) {
  if (action >= HERMES_AVATAR_ANIMATION_COUNT)
    action = ACTION_IDLE;
  return &g_hermes_avatar_animations[action];
}

static const hermes_avatar_asset_step_t *
step_at(const hermes_avatar_t *avatar) {
  const hermes_avatar_asset_animation_t *animation =
      animation_at(avatar->action);
  uint8_t step = avatar->step < animation->step_count ? avatar->step : 0;
  return &g_hermes_avatar_steps[animation->first_step + step];
}

static void set_action(hermes_avatar_t *avatar, uint8_t action,
                       uint32_t now_ms) {
  uint8_t from = step_at(avatar)->expression;
  avatar->action =
      action < HERMES_AVATAR_ANIMATION_COUNT ? action : ACTION_IDLE;
  avatar->step = 0;
  avatar->from_expression = from;
  avatar->phase_started_ms = now_ms;
  avatar->transition_phase = true;
  const hermes_avatar_asset_animation_t *animation =
      animation_at(avatar->action);
  avatar->next_blink_ms = now_ms + animation->blink_initial_delay_ms;
  avatar->blink_started_ms = 0;
}

static uint8_t choose_state_routine(hermes_avatar_t *avatar,
                                    hermes_avatar_state_t state) {
  const hermes_avatar_asset_state_t *pool = &g_hermes_avatar_states[state];
  uint8_t choice =
      (uint8_t)(random_next(avatar) % (uint32_t)pool->routine_count);
  uint8_t action = g_hermes_avatar_state_routines[pool->first_routine + choice];
  if (pool->routine_count > 1 && action == avatar->action) {
    choice = (uint8_t)((choice + 1U) % pool->routine_count);
    action = g_hermes_avatar_state_routines[pool->first_routine + choice];
  }
  return action;
}

static void enter_state(hermes_avatar_t *avatar, hermes_avatar_state_t state,
                        uint32_t now_ms) {
  avatar->state = (uint8_t)state;
  avatar->requested_state = (uint8_t)state;
  avatar->state_change_pending = false;
  set_action(avatar, choose_state_routine(avatar, state), now_ms);
  ESP_LOGI(TAG, "state=%s routine=%s", g_hermes_avatar_states[state].name,
           animation_at(avatar->action)->key);
}

bool hermes_avatar_set_state(hermes_avatar_t *avatar,
                             hermes_avatar_state_t state, uint32_t now_ms) {
  if (!avatar || state >= HERMES_AVATAR_STATE_COUNT)
    return false;
  if (state == HERMES_AVATAR_STATE_IDLE || state == HERMES_AVATAR_STATE_BUSY ||
      state == HERMES_AVATAR_STATE_THINKING)
    avatar->last_activity_ms = now_ms;
  avatar->requested_state = (uint8_t)state;
  if ((uint8_t)state == avatar->state) {
    avatar->state_change_pending = false;
    return true;
  }
  if (avatar->transition_phase) {
    avatar->state_change_pending = true;
    return true;
  }
  enter_state(avatar, state, now_ms);
  return true;
}

bool hermes_avatar_play(hermes_avatar_t *avatar, const char *animation_key,
                        uint32_t now_ms) {
  if (!avatar || !animation_key)
    return false;
  for (uint8_t index = 0; index < HERMES_AVATAR_ANIMATION_COUNT; ++index) {
    if (strcmp(g_hermes_avatar_animations[index].key, animation_key) == 0) {
      set_action(avatar, index, now_ms);
      return true;
    }
  }
  return false;
}

void hermes_avatar_init(hermes_avatar_t *avatar, uint32_t now_ms) {
  if (!avatar)
    return;
  memset(avatar, 0, sizeof(*avatar));
  avatar->last_activity_ms = now_ms;
  avatar->last_frame_ms = now_ms;
  avatar->random_state = 0x4845524DU ^ now_ms;
  avatar->action = ACTION_IDLE;
  avatar->state = HERMES_AVATAR_STATE_IDLE;
  avatar->requested_state = HERMES_AVATAR_STATE_IDLE;
  avatar->from_expression = 0; /* neutral */
  set_action(avatar, ACTION_WAKING, now_ms);
  avatar->from_expression = 0;
}

void hermes_avatar_note_activity(hermes_avatar_t *avatar, uint32_t now_ms) {
  if (!avatar)
    return;
  avatar->last_activity_ms = now_ms;
  if (avatar->state == HERMES_AVATAR_STATE_REST ||
      avatar->requested_state == HERMES_AVATAR_STATE_REST)
    hermes_avatar_set_state(avatar, HERMES_AVATAR_STATE_IDLE, now_ms);
}

static void advance_timeline(hermes_avatar_t *avatar, uint32_t now_ms) {
  for (int safety = 0; safety < 8; ++safety) {
    const hermes_avatar_asset_animation_t *animation =
        animation_at(avatar->action);
    const hermes_avatar_asset_step_t *step = step_at(avatar);
    uint32_t duration =
        avatar->transition_phase ? step->transition_ms : step->hold_ms;
    if (!due(now_ms, avatar->phase_started_ms + duration))
      return;

    avatar->phase_started_ms += duration;
    if (avatar->transition_phase) {
      avatar->transition_phase = false;
      continue;
    }

    uint8_t previous_expression = step->expression;
    uint8_t next_step = (uint8_t)(avatar->step + 1U);
    if (next_step >= animation->step_count) {
      set_action(
          avatar,
          choose_state_routine(avatar, (hermes_avatar_state_t)avatar->state),
          avatar->phase_started_ms);
      continue;
    }

    avatar->from_expression = previous_expression;
    avatar->step = next_step;
    avatar->transition_phase = true;
  }
}

bool hermes_avatar_tick(hermes_avatar_t *avatar, uint32_t now_ms) {
  if (!avatar)
    return false;
  uint32_t idle_for = due(now_ms, avatar->last_activity_ms)
                          ? now_ms - avatar->last_activity_ms
                          : 0;

  if (avatar->state == HERMES_AVATAR_STATE_IDLE &&
      idle_for >= HERMES_AVATAR_REST_AFTER_MS)
    hermes_avatar_set_state(avatar, HERMES_AVATAR_STATE_REST, now_ms);

  advance_timeline(avatar, now_ms);
  if (avatar->state_change_pending && !avatar->transition_phase)
    enter_state(avatar, (hermes_avatar_state_t)avatar->requested_state, now_ms);

  const hermes_avatar_asset_animation_t *animation =
      animation_at(avatar->action);
  if (animation->blink_enabled && due(now_ms, avatar->next_blink_ms)) {
    avatar->blink_started_ms = now_ms;
    uint32_t spread =
        animation->blink_max_interval_ms - animation->blink_min_interval_ms;
    uint32_t interval = animation->blink_min_interval_ms;
    if (spread)
      interval += random_next(avatar) % (spread + 1U);
    avatar->next_blink_ms = now_ms + animation->blink_duration_ms + interval;
  }
  if (avatar->blink_started_ms &&
      due(now_ms, avatar->blink_started_ms + animation->blink_duration_ms))
    avatar->blink_started_ms = 0;

  if (now_ms - avatar->last_frame_ms < FRAME_INTERVAL_MS)
    return false;
  avatar->last_frame_ms = now_ms;
  return true;
}

const char *hermes_avatar_action_name(const hermes_avatar_t *avatar) {
  return animation_at(avatar ? avatar->action : ACTION_IDLE)->key;
}

const char *hermes_avatar_state_name(const hermes_avatar_t *avatar) {
  uint8_t state = avatar ? avatar->state : HERMES_AVATAR_STATE_IDLE;
  if (state >= HERMES_AVATAR_STATE_COUNT)
    state = HERMES_AVATAR_STATE_IDLE;
  return g_hermes_avatar_states[state].name;
}

static frame_decoder_t decoder_for(uint16_t frame) {
  if (frame >= HERMES_AVATAR_FRAME_COUNT)
    frame = 0;
  uint32_t start = g_hermes_avatar_frame_offsets[frame];
  uint32_t end = g_hermes_avatar_frame_offsets[frame + 1U];
  return (frame_decoder_t){
      .cursor = g_hermes_avatar_frame_data + start,
      .end = g_hermes_avatar_frame_data + end,
  };
}

static uint16_t decoder_next(frame_decoder_t *decoder) {
  if (!decoder->remaining) {
    if (decoder->cursor >= decoder->end)
      return 0;
    uint8_t command = *decoder->cursor++;
    decoder->run = (command & 0x80U) != 0;
    decoder->remaining = (uint16_t)((command & 0x7fU) + 1U);
    if (decoder->run) {
      if (decoder->cursor >= decoder->end)
        return 0;
      if (decoder->cursor + 1 >= decoder->end)
        return 0;
      decoder->value = decoder->cursor[0] | ((uint16_t)decoder->cursor[1] << 8);
      decoder->cursor += 2;
    }
  }
  decoder->remaining--;
  if (decoder->run)
    return decoder->value;
  if (decoder->cursor + 1 >= decoder->end)
    return 0;
  uint16_t value = decoder->cursor[0] | ((uint16_t)decoder->cursor[1] << 8);
  decoder->cursor += 2;
  return value;
}

static uint16_t blend_rgb565(uint16_t destination, uint16_t source,
                             uint8_t alpha) {
  if (!alpha)
    return destination;
  if (alpha == 255)
    return source;
  uint32_t inverse = 255U - alpha;
  uint32_t dr = (destination >> 11) & 0x1fU;
  uint32_t dg = (destination >> 5) & 0x3fU;
  uint32_t db = destination & 0x1fU;
  uint32_t sr = (source >> 11) & 0x1fU;
  uint32_t sg = (source >> 5) & 0x3fU;
  uint32_t sb = source & 0x1fU;
  uint32_t r = (dr * inverse + sr * alpha + 127U) / 255U;
  uint32_t g = (dg * inverse + sg * alpha + 127U) / 255U;
  uint32_t b = (db * inverse + sb * alpha + 127U) / 255U;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint8_t mix_channel(uint8_t from, uint8_t to, uint8_t mix) {
  return (uint8_t)(((uint16_t)from * (255U - mix) + (uint16_t)to * mix + 127U) /
                   255U);
}

static uint8_t easing(uint8_t kind, uint8_t linear) {
  float progress = (float)linear / 255.0f;
  float result = progress;
  if (kind == HERMES_AVATAR_SMOOTH) {
    result = progress * progress * (3.0f - 2.0f * progress);
  } else if (kind == HERMES_AVATAR_SNAPPY) {
    float inverse = 1.0f - progress;
    result = 1.0f - inverse * inverse * inverse;
  } else {
    float end = 1.0f - expf(-6.0f) * cosf(8.0f);
    result = (1.0f - expf(-6.0f * progress) * cosf(8.0f * progress)) / end;
    if (result < 0.0f)
      result = 0.0f;
    if (result > 1.0f)
      result = 1.0f;
  }
  return (uint8_t)(result * 255.0f + 0.5f);
}

static const hermes_avatar_asset_transition_t *
find_transition(uint8_t from, uint8_t to, uint8_t kind) {
  for (size_t index = 0; index < HERMES_AVATAR_TRANSITION_COUNT; ++index) {
    const hermes_avatar_asset_transition_t *transition =
        &g_hermes_avatar_transitions[index];
    if (transition->from_expression == from &&
        transition->to_expression == to && transition->kind == kind)
      return transition;
  }
  return NULL;
}

static uint8_t blink_opacity(const hermes_avatar_t *avatar, uint32_t now_ms) {
  if (!avatar->blink_started_ms)
    return 255;
  const hermes_avatar_asset_animation_t *animation =
      animation_at(avatar->action);
  uint32_t elapsed = now_ms - avatar->blink_started_ms;
  if (!animation->blink_duration_ms || elapsed >= animation->blink_duration_ms)
    return 255;
  uint32_t progress = elapsed * 510U / animation->blink_duration_ms;
  return (uint8_t)(progress <= 255U ? 255U - progress : progress - 255U);
}

static uint16_t contrasting_eye_color(void) {
  uint32_t red = (COL_BG >> 11) & 0x1fU;
  uint32_t green = (COL_BG >> 5) & 0x3fU;
  uint32_t blue = COL_BG & 0x1fU;
  uint32_t lightness = red * 2U + green * 3U + blue;
  return lightness > 160U ? COL_TEXT : COL_BG;
}

static void draw_frames(hermes_gfx_t *gfx, uint16_t frame_a, uint16_t frame_b,
                        uint8_t mix, int left, int top, uint8_t eye_opacity) {
  frame_decoder_t decoder_a = decoder_for(frame_a);
  frame_decoder_t decoder_b = decoder_for(frame_b);
  uint16_t eye_color = contrasting_eye_color();

  for (int y = 0; y < HERMES_AVATAR_ASSET_HEIGHT; ++y) {
    int destination_y = top + y;
    for (int x = 0; x < HERMES_AVATAR_ASSET_WIDTH; ++x) {
      uint16_t a = decoder_next(&decoder_a);
      uint16_t b = frame_a == frame_b ? a : decoder_next(&decoder_b);
      uint8_t body = mix_channel(a & 0xffU, b & 0xffU, mix);
      uint8_t eyes = mix_channel(a >> 8, b >> 8, mix);
      eyes = (uint8_t)(((uint16_t)eyes * eye_opacity + 127U) / 255U);
      if (!body && !eyes)
        continue;
      int destination_x = left + x;
      if (destination_x < 0 || destination_x >= GFX_W || destination_y < 0 ||
          destination_y >= GFX_H)
        continue;
      uint16_t *pixel = &gfx->fb[destination_y * GFX_W + destination_x];
      if (body)
        *pixel = blend_rgb565(*pixel, COL_FOCUS, body);
      if (eyes)
        *pixel = blend_rgb565(*pixel, eye_color, eyes);
    }
  }
}

void hermes_avatar_draw(hermes_gfx_t *gfx, const hermes_avatar_t *avatar,
                        int cx, int cy, uint32_t now_ms) {
  if (!gfx || !gfx->fb || !avatar)
    return;
  const hermes_avatar_asset_step_t *step = step_at(avatar);
  uint16_t frame_a = step->expression;
  uint16_t frame_b = frame_a;
  uint8_t mix = 0;

  if (avatar->transition_phase && step->transition_ms) {
    uint32_t elapsed = now_ms - avatar->phase_started_ms;
    if (elapsed < step->transition_ms) {
      uint32_t slot = elapsed * (HERMES_AVATAR_TRANSITION_SAMPLES + 1U) /
                      step->transition_ms;
      const hermes_avatar_asset_transition_t *transition = find_transition(
          avatar->from_expression, step->expression, step->transition);
      if (transition && slot > 0 && slot <= HERMES_AVATAR_TRANSITION_SAMPLES) {
        frame_a = (uint16_t)(transition->first_frame + slot - 1U);
        frame_b = frame_a;
      } else if (slot == 0) {
        frame_a = avatar->from_expression;
        frame_b = frame_a;
      } else if (transition) {
        frame_a = step->expression;
        frame_b = frame_a;
      } else {
        frame_a = avatar->from_expression;
        frame_b = step->expression;
        uint32_t linear = elapsed * 255U / step->transition_ms;
        mix =
            easing(step->transition, (uint8_t)(linear > 255U ? 255U : linear));
      }
    }
  }

  draw_frames(gfx, frame_a, frame_b, mix, cx - HERMES_AVATAR_ASSET_WIDTH / 2,
              cy - HERMES_AVATAR_ASSET_HEIGHT / 2,
              blink_opacity(avatar, now_ms));
}
