#pragma once

#include "driver/gpio.h"

// Board definition. One shared I2C bus carries the peripherals; each
// driver probes for its device at init and disables itself when absent,
// so a bare module still boots into a working console + wallet.
//
// PN7160-specific pins (IRQ/VEN/DWL) live with the driver in nci.h.

#if CONFIG_NUCULA_BOARD_NUCULA_V2

// Nucula v2 (ESP32-C3-WROOM-02-N4): external pull-ups to 3.0 V, so the
// internal ones stay off. NFC and keypad probe at boot; the OLED boost
// is held off during prototype bring-up (see display.cpp).
#define BOARD_I2C_SDA_PIN   GPIO_NUM_4
#define BOARD_I2C_SCL_PIN   GPIO_NUM_5
#define BOARD_I2C_INTERNAL_PULLUP 0

// SSD1309 OLED (SA0 low = 0x3C, SA0 high = 0x3D)
#define BOARD_OLED_ADDR     0x3C
// Boost input power: high enables. Reset: high asserts through Q5.
#define BOARD_OLED_POWER_PIN GPIO_NUM_3
#define BOARD_OLED_RST_PIN  GPIO_NUM_10

#elif CONFIG_NUCULA_BOARD_ATOM

// M5Stack Atom Matrix + MFRC522 over the Grove port (same wiring as the
// ccid-firmware-rs esp32-ccid board-m5atom rig).
#define BOARD_I2C_SDA_PIN   GPIO_NUM_26  // Grove SDA
#define BOARD_I2C_SCL_PIN   GPIO_NUM_32  // Grove SCL
#define BOARD_I2C_INTERNAL_PULLUP 1
#define BOARD_MFRC522_ADDR  0x28
#define BOARD_OLED_RST_PIN  ((gpio_num_t)-1)  // no OLED on this target

#elif CONFIG_NUCULA_BOARD_M5STICK

// M5StickC Plus + MFRC522 on the Grove I2C port. The ST7789 panel rides
// SPI2; its power rails come from the AXP192 PMU on a separate I2C1 bus.
// Plus2 variant (no PMU): DC moves to G14, RST to G12, backlight to G27,
// and GPIO4 must be held high to keep the regulator on — resolved at
// runtime by probing for the PMU; see display_st7789.c.
#define BOARD_I2C_SDA_PIN   GPIO_NUM_32  // Grove SDA
#define BOARD_I2C_SCL_PIN   GPIO_NUM_33  // Grove SCL
#define BOARD_I2C_INTERNAL_PULLUP 1
#define BOARD_MFRC522_ADDR  0x28
#define BOARD_OLED_RST_PIN  ((gpio_num_t)-1)  // no OLED on this target

#define BOARD_PMU_SDA_PIN   GPIO_NUM_21  // I2C1, AXP192 on the Plus
#define BOARD_PMU_SCL_PIN   GPIO_NUM_22
#define BOARD_PMU_ADDR      0x34

// ST7789 display (SPI2), M5StickC Plus pinout
#define BOARD_LCD_SCLK_PIN  GPIO_NUM_13
#define BOARD_LCD_MOSI_PIN  GPIO_NUM_15
#define BOARD_LCD_CS_PIN    GPIO_NUM_5
#define BOARD_LCD_DC_PIN    GPIO_NUM_23
#define BOARD_LCD_RST_PIN   GPIO_NUM_18
#define BOARD_LCD_BL_PIN    GPIO_NUM_27  // Plus2 only; Plus uses AXP192 LDO2
#define BOARD_LCD_WIDTH     135
#define BOARD_LCD_HEIGHT    240
#define BOARD_LCD_OFFSET_X  52
#define BOARD_LCD_OFFSET_Y  40

// Buttons (input-only GPIOs; the board carries external pull-ups)
#define BOARD_BTN_A_PIN     GPIO_NUM_37  // front
#define BOARD_BTN_B_PIN     GPIO_NUM_39  // side

#else // XIAO ESP32-C3

#define BOARD_I2C_SDA_PIN   GPIO_NUM_6   // D4
#define BOARD_I2C_SCL_PIN   GPIO_NUM_7   // D5
#define BOARD_I2C_INTERNAL_PULLUP 1

// SSD1309 OLED (SA0 low = 0x3C, SA0 high = 0x3D)
#define BOARD_OLED_ADDR     0x3C
// Optional hardware reset pin (XIAO D3). Set to -1 if not wired.
#define BOARD_OLED_RST_PIN  GPIO_NUM_5

#endif

// Peripherals below probe for their device on the shared bus and disable
// themselves when absent, so the constants stay board-independent.

// SSD1309 OLED (SA0 low = 0x3C, SA0 high = 0x3D)
#define BOARD_OLED_ADDR     0x3C

// PCF8574 keypad I/O expander
#define BOARD_KEYPAD_ADDR   0x20
