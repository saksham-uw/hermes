#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board_pins.h"

#define GFX_W HERMES_UI_W
#define GFX_H HERMES_UI_H

#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/* Live theme colors (set by hermes_theme_apply). */
extern uint16_t COL_BG;
extern uint16_t COL_TEXT;
extern uint16_t COL_DIM;
extern uint16_t COL_FOCUS;
extern uint16_t COL_OK;
extern uint16_t COL_WARN;
extern uint16_t COL_BAD;
extern uint16_t COL_LINE;

typedef enum {
  THEME_NIGHT = 0,     /* cyber green on black */
  THEME_TERMINAL,      /* phosphor CRT */
  THEME_ABYSS,         /* deep navy + ice */
  THEME_LIGHT,         /* clean cool white */
  THEME_SUMI,          /* mono ink */
  THEME_PORCELAIN,     /* soft slate on paper */
  THEME_FOG,           /* mist blue-grey */
  THEME_COUNT
} hermes_theme_t;

typedef struct {
  uint16_t *fb;
} hermes_gfx_t;

void hermes_theme_apply(hermes_theme_t t);
const char *hermes_theme_name(hermes_theme_t t);

bool hermes_gfx_init(hermes_gfx_t *g);
void hermes_gfx_clear(hermes_gfx_t *g, uint16_t color);
void hermes_gfx_fill_rect(hermes_gfx_t *g, int x, int y, int w, int h, uint16_t color);
void hermes_gfx_hline(hermes_gfx_t *g, int x, int y, int w, uint16_t color);
void hermes_gfx_vline(hermes_gfx_t *g, int x, int y, int h, uint16_t color);
void hermes_gfx_rect(hermes_gfx_t *g, int x, int y, int w, int h, uint16_t color);
void hermes_gfx_pixel(hermes_gfx_t *g, int x, int y, uint16_t color);
void hermes_gfx_char(hermes_gfx_t *g, int x, int y, char c, uint16_t fg, int scale);
void hermes_gfx_text(hermes_gfx_t *g, int x, int y, const char *s, uint16_t fg, int scale);
int hermes_gfx_text_width(const char *s, int scale);
void hermes_gfx_text_centered(hermes_gfx_t *g, int y, const char *s, uint16_t fg, int scale);
int hermes_gfx_text_wrap(hermes_gfx_t *g, int x, int y, int max_w, const char *s, uint16_t fg,
                         int scale);
/* Wrap with newlines preserved. skip_lines scrolls; max_h clips. Returns total lines. */
int hermes_gfx_text_wrap_clip(hermes_gfx_t *g, int x, int y, int max_w, int max_h, int skip_lines,
                             const char *s, uint16_t fg, int scale);
/* Draw design-doc HERMES FIGlet logo. Returns y below logo. */
int hermes_gfx_draw_hermes_logo(hermes_gfx_t *g, int x, int y, uint16_t fg, int scale);
void hermes_gfx_flush(hermes_gfx_t *g);
