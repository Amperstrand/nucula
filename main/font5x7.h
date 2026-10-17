#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 5x7 bitmap font, ASCII 32-126, 5 bytes per glyph, column-major with
// bit 0 = topmost pixel.

#define FONT_W       5
#define FONT_H       8
#define FONT_ADVANCE 6 // glyph + 1px spacing

// Column bytes for a printable character; '?' for anything else.
const uint8_t *font5x7_glyph(char c);

static inline bool font5x7_pixel(const uint8_t *glyph, int col, int row)
{
    return col < FONT_W && (glyph[col] & (1 << row)) != 0;
}

#ifdef __cplusplus
}
#endif
