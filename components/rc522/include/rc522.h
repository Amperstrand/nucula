#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// MFRC522 reader-mode driver over I2C (ISO14443-3 Type A + MIFARE
// Ultralight/NTAG page reads).
//
// Register-level behaviour is a C transliteration of the in-house
// Amperstrand/mfrc522-rs fork (hardware-proven on the M5 Atom +
// MFRC522 rig shared with ccid-firmware-rs and bolty-rs).

// A selected ISO14443-3 Type A tag.
typedef struct {
    uint8_t uid[10];
    uint8_t uid_len; // 4 (single), 7 (double) or 10 (triple)
    uint8_t sak;
} rc522_tag_t;

// Add the MFRC522 as a device on the shared I2C bus and run the init
// sequence (soft reset, timer/CRC configuration, antenna on).
// Fails if the bus handle is NULL or the device cannot be added.
esp_err_t rc522_init(i2c_master_bus_handle_t bus, uint8_t addr);

// True when VersionReg reads a plausible silicon version (0x91/0x92).
bool rc522_probe(void);

// REQA + full anticollision + SELECT. Returns false when no tag answers
// (or the exchange fails); out is filled on success.
bool rc522_poll(rc522_tag_t *out);

// MIFARE Ultralight / NTAG READ: reads 4 pages starting at `page`
// (16 bytes, CRC-checked). The ACR1252 card emulation answers these.
bool rc522_ul_read(uint8_t page, uint8_t out[16]);

#ifdef __cplusplus
}
#endif
