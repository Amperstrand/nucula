#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_NUCULA_BOARD_M5STICK
#include "board.h"
#define LCD_W BOARD_LCD_WIDTH
#define LCD_H BOARD_LCD_HEIGHT
#else
#define LCD_W 128
#define LCD_H 64
#endif

// Monochrome: non-zero = pixel on, zero = pixel off.
#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0x0001

void display_init(i2c_master_bus_handle_t bus);

void display_clear(void);
void display_update(void);

void display_fill_rect(int x, int y, int w, int h, uint16_t color);

void display_text(int x, int y, const char *text, int scale);
void display_text_color(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale);

int  display_text_width(const char *text, int scale);

void display_hline(int x, int y, int w);
void display_pixel(int x, int y, bool on);

#ifdef __cplusplus
}
#endif

