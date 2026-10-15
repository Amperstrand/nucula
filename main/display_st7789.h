#pragma once

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// M5StickC Plus ST7789 panel via the IDF esp_lcd panel stack.
// display_st7789_init sequences the power rails first (AXP192 on the
// Plus, GPIO-driven on the Plus2 — resolved by PMU probe), then SPI2
// and the panel itself. Pixel colors are RGB565; every draw applies
// the panel's (52, 40) offset.

// Plus = AXP192 rails + backlight on LDO2; Plus2 = GPIO4 power-hold +
// backlight on GPIO27. Reported once at init for console diagnostics.
typedef enum {
    ST7789_VARIANT_NONE = 0,
    ST7789_VARIANT_PLUS,
    ST7789_VARIANT_PLUS2,
} st7789_variant_t;

esp_err_t display_st7789_init(void);
st7789_variant_t display_st7789_variant(void);

// Direct RGB565 primitives (diagnostics, future rich UI).
void display_st7789_fill(uint16_t color);
void display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_st7789_set_pixel(int x, int y, uint16_t color);
void display_st7789_backlight(bool on);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_NUCULA_BOARD_M5STICK
