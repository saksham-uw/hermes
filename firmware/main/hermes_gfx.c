#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hermes_gfx.h"
#include "display_axs15231b.h"

static const char *TAG = "hermes_gfx";

uint16_t COL_BG = RGB565(0, 0, 0);
uint16_t COL_TEXT = RGB565(236, 236, 236);
uint16_t COL_DIM = RGB565(110, 110, 110);
uint16_t COL_FOCUS = RGB565(232, 140, 64);
uint16_t COL_OK = RGB565(90, 170, 110);
uint16_t COL_WARN = RGB565(220, 150, 70);
uint16_t COL_BAD = RGB565(200, 80, 80);
uint16_t COL_LINE = RGB565(40, 40, 40);

void hermes_theme_apply(hermes_theme_t t) {
  switch (t) {
    case THEME_TERMINAL:
      COL_BG = RGB565(4, 12, 6);
      COL_TEXT = RGB565(120, 255, 140);
      COL_DIM = RGB565(40, 110, 55);
      COL_FOCUS = RGB565(180, 255, 120);
      COL_OK = RGB565(80, 220, 100);
      COL_WARN = RGB565(200, 200, 60);
      COL_BAD = RGB565(255, 80, 80);
      COL_LINE = RGB565(20, 50, 28);
      break;
    case THEME_ABYSS:
      COL_BG = RGB565(6, 10, 28);
      COL_TEXT = RGB565(210, 225, 245);
      COL_DIM = RGB565(90, 110, 150);
      COL_FOCUS = RGB565(100, 210, 230);
      COL_OK = RGB565(70, 190, 160);
      COL_WARN = RGB565(230, 170, 90);
      COL_BAD = RGB565(230, 90, 120);
      COL_LINE = RGB565(30, 40, 70);
      break;
    case THEME_LIGHT:
      COL_BG = RGB565(248, 249, 252);
      COL_TEXT = RGB565(22, 26, 34);
      COL_DIM = RGB565(120, 128, 140);
      COL_FOCUS = RGB565(30, 100, 180);
      COL_OK = RGB565(30, 140, 90);
      COL_WARN = RGB565(180, 120, 30);
      COL_BAD = RGB565(180, 50, 50);
      COL_LINE = RGB565(210, 214, 222);
      break;
    case THEME_SUMI:
      COL_BG = RGB565(232, 234, 230);
      COL_TEXT = RGB565(16, 18, 22);
      COL_DIM = RGB565(90, 95, 100);
      COL_FOCUS = RGB565(40, 55, 90);
      COL_OK = RGB565(45, 90, 70);
      COL_WARN = RGB565(100, 80, 40);
      COL_BAD = RGB565(120, 40, 45);
      COL_LINE = RGB565(180, 182, 176);
      break;
    case THEME_PORCELAIN:
      COL_BG = RGB565(240, 242, 245);
      COL_TEXT = RGB565(28, 32, 40);
      COL_DIM = RGB565(130, 136, 148);
      COL_FOCUS = RGB565(70, 90, 130);
      COL_OK = RGB565(50, 120, 100);
      COL_WARN = RGB565(160, 120, 70);
      COL_BAD = RGB565(150, 60, 70);
      COL_LINE = RGB565(200, 204, 212);
      break;
    case THEME_FOG:
      COL_BG = RGB565(220, 228, 234);
      COL_TEXT = RGB565(30, 40, 52);
      COL_DIM = RGB565(100, 115, 130);
      COL_FOCUS = RGB565(50, 120, 150);
      COL_OK = RGB565(40, 130, 110);
      COL_WARN = RGB565(160, 110, 50);
      COL_BAD = RGB565(160, 55, 65);
      COL_LINE = RGB565(180, 192, 200);
      break;
    case THEME_NIGHT:
    default:
      COL_BG = RGB565(0, 0, 0);
      COL_TEXT = RGB565(236, 236, 236);
      COL_DIM = RGB565(110, 110, 110);
      COL_FOCUS = RGB565(80, 220, 160);
      COL_OK = RGB565(90, 170, 110);
      COL_WARN = RGB565(220, 150, 70);
      COL_BAD = RGB565(200, 80, 80);
      COL_LINE = RGB565(40, 40, 40);
      break;
  }
}

const char *hermes_theme_name(hermes_theme_t t) {
  switch (t) {
    case THEME_TERMINAL: return "terminal";
    case THEME_ABYSS: return "abyss";
    case THEME_LIGHT: return "light";
    case THEME_SUMI: return "sumi ink";
    case THEME_PORCELAIN: return "porcelain";
    case THEME_FOG: return "fog";
    default: return "night";
  }
}

/* 5x7 glyphs ASCII 32..126 */
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x08, 0x2A, 0x1C, 0x2A, 0x08}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x00, 0x08, 0x14, 0x22, 0x41}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x41, 0x22, 0x14, 0x08, 0x00}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x01, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x32}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x24},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x04, 0x08, 0x10, 0x08},
};

static uint16_t *s_tx; /* DMA bounce — native rows */
static size_t s_tx_rows;

static inline uint16_t panel_swap(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* RSVP LandscapeFlipped: logicalX = physicalY; logicalY = UI_H-1-physicalX */
static inline void map_phys_to_logic(int px, int py, int *lx, int *ly) {
  *lx = py;
  *ly = HERMES_UI_H - 1 - px;
}

bool hermes_gfx_init(hermes_gfx_t *g) {
  if (!g) return false;
  size_t bytes = (size_t)GFX_W * GFX_H * sizeof(uint16_t);
  g->fb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g->fb) g->fb = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  if (!g->fb) return false;

  /* Max rows that fit in 16KB DMA chunk at native width 172. */
  s_tx_rows = (16 * 1024) / (HERMES_PANEL_W * sizeof(uint16_t));
  if (s_tx_rows < 1) s_tx_rows = 1;
  size_t tx_bytes = s_tx_rows * HERMES_PANEL_W * sizeof(uint16_t);
  s_tx = heap_caps_malloc(tx_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!s_tx) {
    ESP_LOGE(TAG, "DMA tx buffer alloc failed");
    return false;
  }
  ESP_LOGI(TAG, "fb %dx%d + DMA tx %u rows", GFX_W, GFX_H, (unsigned)s_tx_rows);
  return true;
}

void hermes_gfx_clear(hermes_gfx_t *g, uint16_t color) {
  if (!g || !g->fb) return;
  for (size_t i = 0; i < (size_t)GFX_W * GFX_H; i++) g->fb[i] = color;
}

void hermes_gfx_pixel(hermes_gfx_t *g, int x, int y, uint16_t color) {
  if (!g || !g->fb || x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) return;
  g->fb[y * GFX_W + x] = color;
}

void hermes_gfx_fill_rect(hermes_gfx_t *g, int x, int y, int w, int h, uint16_t color) {
  if (!g || !g->fb || w <= 0 || h <= 0) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > GFX_W) w = GFX_W - x;
  if (y + h > GFX_H) h = GFX_H - y;
  for (int yy = y; yy < y + h; yy++) {
    uint16_t *row = &g->fb[yy * GFX_W + x];
    for (int xx = 0; xx < w; xx++) row[xx] = color;
  }
}

void hermes_gfx_hline(hermes_gfx_t *g, int x, int y, int w, uint16_t color) {
  hermes_gfx_fill_rect(g, x, y, w, 1, color);
}

void hermes_gfx_vline(hermes_gfx_t *g, int x, int y, int h, uint16_t color) {
  hermes_gfx_fill_rect(g, x, y, 1, h, color);
}

void hermes_gfx_rect(hermes_gfx_t *g, int x, int y, int w, int h, uint16_t color) {
  hermes_gfx_hline(g, x, y, w, color);
  hermes_gfx_hline(g, x, y + h - 1, w, color);
  hermes_gfx_vline(g, x, y, h, color);
  hermes_gfx_vline(g, x + w - 1, y, h, color);
}

int hermes_gfx_draw_hermes_logo(hermes_gfx_t *g, int x, int y, uint16_t fg, int scale) {
  /* Exact design_doc FIGlet (byte-for-byte). */
  static const char *lines[] = {
      " _   _  ____  ____  __  __  ____  ___ ",
      "( )_( )( ___)(  _ \\(  \\/  )( ___)/ __)",
      " ) _ (  )__)  )   / )    (  )__) \\__ \\",
      "(_) (_)(____)(_)\\_)(_/\\/\\_)(____)(___/",
  };
  int line_h = 8 * scale;
  for (int i = 0; i < 4; i++) {
    hermes_gfx_text(g, x, y + i * line_h, lines[i], fg, scale);
  }
  return y + 4 * line_h;
}

void hermes_gfx_char(hermes_gfx_t *g, int x, int y, char c, uint16_t fg, int scale) {
  if (scale < 1) scale = 1;
  if (c < 32 || c > 126) c = '?';
  const uint8_t *glyph = font5x7[c - 32];
  for (int col = 0; col < 5; col++) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; row++) {
      if (!(bits & (1 << row))) continue;
      if (scale == 1) hermes_gfx_pixel(g, x + col, y + row, fg);
      else hermes_gfx_fill_rect(g, x + col * scale, y + row * scale, scale, scale, fg);
    }
  }
}

int hermes_gfx_text_width(const char *s, int scale) {
  if (!s) return 0;
  int n = 0;
  while (*s && *s != '\n') {
    n++;
    s++;
  }
  return n * 6 * scale;
}

void hermes_gfx_text(hermes_gfx_t *g, int x, int y, const char *s, uint16_t fg, int scale) {
  if (!s) return;
  int cx = x;
  int advance = 6 * scale;
  while (*s) {
    if (*s == '\n') {
      cx = x;
      y += 8 * scale;
      s++;
      continue;
    }
    hermes_gfx_char(g, cx, y, *s, fg, scale);
    cx += advance;
    s++;
  }
}

void hermes_gfx_text_centered(hermes_gfx_t *g, int y, const char *s, uint16_t fg, int scale) {
  int w = hermes_gfx_text_width(s, scale);
  hermes_gfx_text(g, (GFX_W - w) / 2, y, s, fg, scale);
}

int hermes_gfx_text_wrap_clip(hermes_gfx_t *g, int x, int y0, int max_w, int max_h, int skip_lines,
                             const char *s, uint16_t fg, int scale) {
  if (!s || !s[0]) return 0;
  if (scale < 1) scale = 1;
  int advance = 6 * scale;
  int line_h = 8 * scale;
  int cols = max_w / advance;
  if (cols < 1) cols = 1;
  int line = 0;
  int col = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '\r') continue;
    if (*p == '\n') {
      line++;
      col = 0;
      continue;
    }
    if (col >= cols) {
      line++;
      col = 0;
    }
    if (g && line >= skip_lines) {
      int y = y0 + (line - skip_lines) * line_h;
      if (max_h <= 0 || y + line_h <= y0 + max_h) {
        hermes_gfx_char(g, x + col * advance, y, *p, fg, scale);
      }
    }
    col++;
  }
  return line + 1;
}

int hermes_gfx_text_wrap(hermes_gfx_t *g, int x, int y, int max_w, const char *s, uint16_t fg,
                         int scale) {
  int lines = hermes_gfx_text_wrap_clip(g, x, y, max_w, 0, 0, s, fg, scale);
  return y + lines * 8 * (scale < 1 ? 1 : scale);
}

void hermes_gfx_flush(hermes_gfx_t *g) {
  if (!g || !g->fb || !s_tx || !hermes_display_ready()) return;

  /* Rotate logical landscape → native portrait rows, DMA-safe bounce buffer. */
  for (int native_y0 = 0; native_y0 < HERMES_PANEL_H; native_y0 += (int)s_tx_rows) {
    int rows = (int)s_tx_rows;
    if (native_y0 + rows > HERMES_PANEL_H) rows = HERMES_PANEL_H - native_y0;

    for (int ly = 0; ly < rows; ly++) {
      int native_y = native_y0 + ly;
      uint16_t *dst = s_tx + (size_t)ly * HERMES_PANEL_W;
      for (int native_x = 0; native_x < HERMES_PANEL_W; native_x++) {
        int lx, lyy;
        map_phys_to_logic(native_x, native_y, &lx, &lyy);
        uint16_t c = 0;
        if (lx >= 0 && lx < GFX_W && lyy >= 0 && lyy < GFX_H) c = g->fb[lyy * GFX_W + lx];
        dst[native_x] = panel_swap(c);
      }
    }
    hermes_display_flush_native(s_tx, (uint16_t)native_y0, (uint16_t)rows);
  }
}
