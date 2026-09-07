#pragma once

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// M5StickC Plus ST7789 panel. display_st7789_init programs the AXP192 PMU
// first (display + Grove power rails), then SPI2 and the panel itself.
// Pixel colors are RGB565; every draw applies the panel's (52, 40) offset.

esp_err_t display_st7789_init(void);
void display_st7789_fill(uint16_t color);
void display_st7789_set_pixel(int x, int y, uint16_t color);
void display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_st7789_backlight(bool on);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_NUCULA_BOARD_M5STICK
