#pragma once

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// AXP192 PMU on the M5StickC Plus (I2C1, 0x34). Nothing on that board
// runs until the PMU turns its rails on:
//   DCDC1  main 3.3 V rail (ESP32, ST7789 logic)
//   LDO2   backlight LEDs
//   LDO3   LCD logic
//   EXTEN  Grove-port 5 V boost the MFRC522 draws its power from
//
// The M5StickC Plus2 has no PMU; axp192_init() then reports absent and
// the display driver falls back to its GPIO-driven variant.

// Probe the PMU and enable the display + Grove rails. Safe to call
// repeatedly; returns ESP_ERR_NOT_FOUND on a Plus2.
esp_err_t axp192_init(void);

// Backlight on the Plus is LDO2 (reg 0x12 bit 2), not a GPIO.
esp_err_t axp192_backlight(bool on);

// EXTEN gates the Grove rail. The PMU holds this rail across ESP32
// resets, so a latched RC522 (SDA stuck LOW, immune to SCL clocking)
// only clears when the rail actually drops.
esp_err_t axp192_grove_power(bool on);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_NUCULA_BOARD_M5STICK
