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

// ---- ISO-DEP (ISO14443-4) layer ----
// Minimal T=CL transport for Type 4 NDEF tags: RATS/ATS negotiation,
// CID-less I-blocks with hardware CRC, card-side chaining (R-ACK) and
// WTX (S-block) handling. Transliterated from the house iso14443-rs
// PCD session logic (bolty-rs lineage).

typedef struct {
    bool hw_crc;       // TxMode/RxMode CRC units active
    uint8_t fsc;       // effective frame size, capped by the 64-byte FIFO
    uint32_t fwt_ms;   // base frame waiting time from ATS TB / FWI
    uint8_t block_nr;  // our I-block sequence bit
} rc522_isodep_t;

// Antenna drivers on/off (TxControlReg TX1RFEn|TX2RFEn). The port keeps
// the field OFF while idle so a co-located writer (the rig's ACR1252)
// can reach the card without field collision.
bool rc522_field(bool on);

// RATS (FSD 64, CID 0) + ATS parse; enables hardware CRC. Requires a
// tag selected by rc522_poll with SAK bit 0x20 (ISO14443-4 compliant).
bool rc522_isodep_connect(rc522_isodep_t *s);

// Drop back to raw framing after a T=CL session.
void rc522_isodep_end(rc522_isodep_t *s);

// Exchange one APDU (must fit one I-block: len+3 <= fsc). Collects the
// card's chained I-blocks and honors WTX. Response INF lands in out.
bool rc522_isodep_exchange(rc522_isodep_t *s,
                           const uint8_t *apdu, size_t apdu_len,
                           uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
