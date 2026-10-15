#pragma once

#include "driver/gpio.h"

// Board definition. One shared I2C bus carries all peripherals; each
// driver probes for its device at init and disables itself when absent,
// so a bare module still boots into a working console + wallet.
//
// PN7160-specific pins (IRQ/VEN/DWL) live with the driver in nci.h.

#if CONFIG_NUCULA_BOARD_ATOM

// M5Stack Atom Matrix + MFRC522 over the Grove port (same wiring as the
// ccid-firmware-rs esp32-ccid board-m5atom rig).
#define BOARD_I2C_SDA_PIN   GPIO_NUM_26  // Grove SDA
#define BOARD_I2C_SCL_PIN   GPIO_NUM_32  // Grove SCL
#define BOARD_MFRC522_ADDR  0x28

#elif CONFIG_NUCULA_BOARD_M5STICK

// M5StickC Plus: Grove I2C port for the MFRC522
#define BOARD_I2C_SDA_PIN   GPIO_NUM_32
#define BOARD_I2C_SCL_PIN   GPIO_NUM_33
#define BOARD_MFRC522_ADDR  0x28

// ST7789 display (SPI2)
#define BOARD_LCD_SCLK_PIN  GPIO_NUM_13
#define BOARD_LCD_MOSI_PIN  GPIO_NUM_15
#define BOARD_LCD_CS_PIN    GPIO_NUM_5
#define BOARD_LCD_DC_PIN    GPIO_NUM_23
#define BOARD_LCD_RST_PIN   GPIO_NUM_18
#define BOARD_LCD_BL_PIN    GPIO_NUM_27

// AXP192 PMU (I2C1 — separate from the NFC I2C bus)
#define BOARD_PMU_SDA_PIN   GPIO_NUM_21
#define BOARD_PMU_SCL_PIN   GPIO_NUM_22
#define BOARD_PMU_ADDR      0x34

// Buttons (input, pull-up, active low)
#define BOARD_BTN_A_PIN     GPIO_NUM_37
#define BOARD_BTN_B_PIN     GPIO_NUM_39

// ST7789 panel geometry
#define BOARD_LCD_WIDTH     135
#define BOARD_LCD_HEIGHT    240
#define BOARD_LCD_OFFSET_X  52
#define BOARD_LCD_OFFSET_Y  40

#elif CONFIG_NUCULA_BOARD_NUCULAPCB

// zeugmaster nucula production PCB (ESP32-C3 + PN7160). Pinout proven on
// the bench by ccid-firmware-rs (pn7160-nci crate transport consts).
#define BOARD_I2C_SDA_PIN   GPIO_NUM_4
#define BOARD_I2C_SCL_PIN   GPIO_NUM_5

// No SSD1309 on this board — and GPIO5 is SCL here: driving an OLED
// reset on it would rip SCL out of I2C mode before nfc_init() probes
// the PN7160 (the "silent PN7160" bench incident, fork issue #12).
#define BOARD_OLED_RST_PIN (-1)

#else // XIAO ESP32-C3

#define BOARD_I2C_SDA_PIN   GPIO_NUM_6   // D4
#define BOARD_I2C_SCL_PIN   GPIO_NUM_7   // D5

#endif

// Peripherals below probe for their device on the shared bus and disable
// themselves when absent, so the constants stay board-independent.

// SSD1309 OLED (SA0 low = 0x3C, SA0 high = 0x3D)
#define BOARD_OLED_ADDR     0x3C
// Optional hardware reset pin (XIAO D3). Set to -1 if not wired.
#ifndef BOARD_OLED_RST_PIN
#define BOARD_OLED_RST_PIN  GPIO_NUM_5
#endif

// PCF8574 keypad I/O expander
#define BOARD_KEYPAD_ADDR   0x20
