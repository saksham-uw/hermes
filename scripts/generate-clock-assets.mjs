import { readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const lines = (await readFile(join(root, "design_doc.txt"), "utf8")).split(/\r?\n/);
const masks = new Map([
  ["▀", 0x3], ["▄", 0xc], ["█", 0xf], ["▌", 0x5], ["▐", 0xa],
  ["▖", 0x4], ["▗", 0x8], ["▘", 0x1], ["▙", 0xd], ["▚", 0x9],
  ["▛", 0x7], ["▜", 0xb], ["▝", 0x2], ["▞", 0x6], ["▟", 0xe],
]);
const names = ["1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "AM", "PM"];
const glyphs = names.map((name, index) => {
  const source = lines.slice(index * 7, index * 7 + 4).map((line) => [...line.replace(/\s+$/u, "")]);
  const width = Math.max(1, ...source.map((line) => line.length));
  const rows = Array.from({ length: 8 }, () => 0);
  source.forEach((line, y) => line.forEach((cell, x) => {
    const mask = masks.get(cell) ?? 0;
    if (mask & 0x1) rows[y * 2] |= 1 << (x * 2);
    if (mask & 0x2) rows[y * 2] |= 1 << (x * 2 + 1);
    if (mask & 0x4) rows[y * 2 + 1] |= 1 << (x * 2);
    if (mask & 0x8) rows[y * 2 + 1] |= 1 << (x * 2 + 1);
  }));
  return { name, width: width * 2, rows };
});

const header = `/* Generated from design_doc.txt. Do not edit by hand. */
#pragma once
#include <stdint.h>

#define HOME_CLOCK_GLYPH_COUNT 12
#define HOME_CLOCK_GLYPH_HEIGHT 8
extern const uint8_t g_home_clock_widths[HOME_CLOCK_GLYPH_COUNT];
extern const uint32_t g_home_clock_rows[HOME_CLOCK_GLYPH_COUNT][HOME_CLOCK_GLYPH_HEIGHT];
`;
const body = `/* Generated from design_doc.txt. Do not edit by hand. */
#include "home_clock_assets.h"

const uint8_t g_home_clock_widths[HOME_CLOCK_GLYPH_COUNT] = {
  ${glyphs.map((glyph) => glyph.width).join(", ")}
};

const uint32_t g_home_clock_rows[HOME_CLOCK_GLYPH_COUNT][HOME_CLOCK_GLYPH_HEIGHT] = {
${glyphs.map((glyph) => `  /* ${glyph.name} */ { ${glyph.rows.map((row) => `0x${row.toString(16)}U`).join(", ")} }`).join(",\n")}
};
`;
await writeFile(join(root, "firmware/main/home_clock_assets.h"), header);
await writeFile(join(root, "firmware/main/home_clock_assets.c"), body);
