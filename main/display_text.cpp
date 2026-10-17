// Backend-agnostic text rendering: glyphs go through display_pixel,
// so the SSD1309 page framebuffer and the ST7789 linear canvas share
// one renderer.

#include "display.h"
#include "font5x7.h"

#include <cstring>

void display_text_color(int x, int y, const char *text,
                        uint16_t fg, uint16_t bg, int scale)
{
    if (!text || !*text) return;
    bool fg_on = (fg != 0);
    bool bg_on = (bg != 0);

    for (; *text; text++, x += FONT_ADVANCE * scale) {
        if (x + FONT_ADVANCE * scale > LCD_W) break;

        const uint8_t *glyph = font5x7_glyph(*text);

        for (int row = 0; row < FONT_H; row++) {
            int py = y + row * scale;
            if (py >= LCD_H) break;
            for (int col = 0; col < FONT_ADVANCE; col++) {
                bool on = font5x7_pixel(glyph, col, row);
                bool pixel = on ? fg_on : bg_on;
                int px = x + col * scale;
                for (int sy = 0; sy < scale && py + sy < LCD_H; sy++)
                    for (int sx = 0; sx < scale && px + sx < LCD_W; sx++)
                        display_pixel(px + sx, py + sy, pixel);
            }
        }
    }
}

void display_text(int x, int y, const char *text, int scale)
{
    display_text_color(x, y, text, COLOR_WHITE, COLOR_BLACK, scale);
}

int display_text_width(const char *text, int scale)
{
    return (int)strlen(text) * FONT_ADVANCE * scale;
}
