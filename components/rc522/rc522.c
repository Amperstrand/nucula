// MFRC522 I2C driver — C transliteration of Amperstrand/mfrc522-rs
// (e9ced1e), the fork proven on this exact Atom + MFRC522 hardware in
// ccid-firmware-rs and bolty-rs. Register map, init values, transceive
// sequencing, timer reload (100 ms fix), and the anticollision bit
// handling all follow the Rust implementation.

#include "rc522.h"

#include <string.h>

#include "esp_log.h"

#define TAG "rc522"

// ---- Registers (MFRC522 datasheet table 20) ----
enum {
    RC522_CommandReg      = 0x01,
    RC522_ComIrqReg       = 0x04,
    RC522_DivIrqReg       = 0x05,
    RC522_ErrorReg        = 0x06,
    RC522_FIFODataReg     = 0x09,
    RC522_FIFOLevelReg    = 0x0A,
    RC522_ControlReg     = 0x0C,
    RC522_BitFramingReg   = 0x0D,
    RC522_CollReg         = 0x0E,
    RC522_ModeReg         = 0x11,
    RC522_TxModeReg       = 0x12,
    RC522_RxModeReg       = 0x13,
    RC522_TxControlReg    = 0x14,
    RC522_TxASKReg        = 0x15,
    RC522_ModWidthReg     = 0x24,
    RC522_TModeReg        = 0x2A,
    RC522_TPrescalerReg   = 0x2B,
    RC522_TReloadRegHigh  = 0x2C,
    RC522_TReloadRegLow   = 0x2D,
    RC522_CRCResultRegHigh = 0x21,
    RC522_CRCResultRegLow = 0x22,
    RC522_VersionReg      = 0x37,
};

// ---- Commands ----
enum {
    RC522_CMD_IDLE      = 0x00,
    RC522_CMD_CALC_CRC  = 0x03,
    RC522_CMD_TRANSCEIVE = 0x0C,
    RC522_CMD_SOFT_RESET = 0x0F,
};

// ---- IRQ / status bits ----
#define RC522_TIMER_IRQ   (1 << 0)
#define RC522_ERR_IRQ     (1 << 1)
#define RC522_IDLE_IRQ    (1 << 4)
#define RC522_RX_IRQ      (1 << 5)
#define RC522_CRC_IRQ     (1 << 2) // DivIrqReg
#define RC522_POWER_DOWN  (1 << 4) // CommandReg
#define RC522_FLUSH_BUFFER (1 << 7) // FIFOLevelReg
#define RC522_FORCE_100_ASK (1 << 6) // TxASKReg

// ---- ErrorReg bits ----
#define RC522_PROTOCOL_ERR (1 << 0)
#define RC522_PARITY_ERR   (1 << 1)
#define RC522_CRC_ERR      (1 << 2)
#define RC522_COLL_ERR     (1 << 3)
#define RC522_BUFFER_OVFL  (1 << 4)

// ---- PICC commands (ISO14443-3 / MIFARE) ----
#define PICC_REQA     0x26
#define PICC_WUPA     0x52
#define PICC_CT       0x88
#define PICC_SEL_CL1  0x93
#define PICC_SEL_CL2  0x95
#define PICC_SEL_CL3  0x97
#define PICC_HLTA     0x50
#define PICC_UL_READ  0x30
#define PICC_MF_READ  0x30

#define RC522_I2C_TIMEOUT_MS 100

static i2c_master_dev_handle_t s_dev;

// ---- I2C register access ----

static bool rc522_rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1,
                                       RC522_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool rc522_rd_many(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       RC522_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool rc522_wr(uint8_t reg, uint8_t val)
{
    uint8_t frame[2] = { reg, val };
    return i2c_master_transmit(s_dev, frame, sizeof(frame),
                               RC522_I2C_TIMEOUT_MS) == ESP_OK;
}

static bool rc522_wr_many(uint8_t reg, const uint8_t *bytes, size_t len)
{
    // MFRC522 auto-increments the register address on consecutive writes.
    uint8_t frame[66];
    if (len + 1 > sizeof(frame))
        return false;
    frame[0] = reg;
    memcpy(&frame[1], bytes, len);
    return i2c_master_transmit(s_dev, frame, len + 1,
                               RC522_I2C_TIMEOUT_MS) == ESP_OK;
}

// ---- Low-level machinery ----

static bool rc522_command(uint8_t cmd)
{
    return rc522_wr(RC522_CommandReg, cmd);
}

static bool rc522_fifo_flush(void)
{
    return rc522_wr(RC522_FIFOLevelReg, RC522_FLUSH_BUFFER);
}

static bool rc522_check_error(void)
{
    uint8_t err;
    if (!rc522_rd(RC522_ErrorReg, &err))
        return false;
    // Any set bit here (except reserved ones) means the exchange failed;
    // callers translate to a plain false.
    if (err & (RC522_PROTOCOL_ERR | RC522_PARITY_ERR | RC522_CRC_ERR |
               RC522_COLL_ERR | RC522_BUFFER_OVFL))
        return false;
    return true;
}

// CRC via the coprocessor (preset 0x6363, per ModeReg in init).
static bool rc522_calc_crc(const uint8_t *data, size_t len, uint8_t crc[2])
{
    if (!rc522_command(RC522_CMD_IDLE))
        return false;
    if (!rc522_wr(RC522_DivIrqReg, RC522_CRC_IRQ))
        return false;
    if (!rc522_fifo_flush())
        return false;
    if (!rc522_wr_many(RC522_FIFODataReg, data, len))
        return false;
    if (!rc522_command(RC522_CMD_CALC_CRC))
        return false;

    for (uint32_t i = 0; i < 5000; i++) {
        uint8_t irq;
        if (!rc522_rd(RC522_DivIrqReg, &irq))
            return false;
        if (irq & RC522_CRC_IRQ) {
            if (!rc522_command(RC522_CMD_IDLE))
                return false;
            uint8_t lo, hi;
            if (!rc522_rd(RC522_CRCResultRegLow, &lo))
                return false;
            if (!rc522_rd(RC522_CRCResultRegHigh, &hi))
                return false;
            crc[0] = lo;
            crc[1] = hi;
            return true;
        }
    }
    return false;
}

// Transceive: send tx (with tx_last_bits valid bits in the final byte),
// receive into rx. Returns the number of received bytes in *rx_len, and
// valid bits of the last received byte in *rx_last_bits.
static bool rc522_transceive(const uint8_t *tx, size_t tx_len,
                              uint8_t tx_last_bits, uint8_t rx_align,
                              uint8_t *rx, size_t rx_cap, size_t *rx_len,
                              uint8_t *rx_last_bits)
{
    if (!rc522_command(RC522_CMD_IDLE))
        return false;
    if (!rc522_wr(RC522_ComIrqReg, 0x7F))
        return false;
    if (!rc522_fifo_flush())
        return false;
    if (!rc522_wr_many(RC522_FIFODataReg, tx, tx_len))
        return false;
    if (!rc522_command(RC522_CMD_TRANSCEIVE))
        return false;
    if (!rc522_wr(RC522_BitFramingReg,
                  (1 << 7) | ((rx_align & 0x07) << 4) | (tx_last_bits & 0x07)))
        return false;

    // The hardware timer (configured to ~100 ms in init) fires TIMER_IRQ
    // on silence; the iteration cap guards against a misconfigured timer.
    for (uint32_t i = 0; i < 5000; i++) {
        uint8_t irq;
        if (!rc522_rd(RC522_ComIrqReg, &irq))
            return false;
        if (irq & (RC522_RX_IRQ | RC522_ERR_IRQ | RC522_IDLE_IRQ))
            break;
        if (irq & RC522_TIMER_IRQ)
            return false; // timeout
    }

    if (!rc522_check_error())
        return false;

    uint8_t level;
    if (!rc522_rd(RC522_FIFOLevelReg, &level))
        return false;
    if (level > rx_cap)
        return false;
    if (level > 0 && !rc522_rd_many(RC522_FIFODataReg, rx, level))
        return false;
    uint8_t ctrl;
    if (!rc522_rd(RC522_ControlReg, &ctrl))
        return false;

    *rx_len = level;
    *rx_last_bits = ctrl & 0x07;
    return true;
}

// ---- Public API ----

esp_err_t rc522_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!bus)
        return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK)
        return err;

    // Soft reset, then wait for the power-down flag to clear.
    if (!rc522_command(RC522_CMD_SOFT_RESET))
        return ESP_ERR_INVALID_STATE;
    for (uint32_t i = 0; i < 5000; i++) {
        uint8_t cmd;
        if (!rc522_rd(RC522_CommandReg, &cmd))
            return ESP_ERR_INVALID_STATE;
        if (!(cmd & RC522_POWER_DOWN))
            break;
    }

    if (!rc522_wr(RC522_TxModeReg, 0x00) ||
        !rc522_wr(RC522_RxModeReg, 0x00) ||
        !rc522_wr(RC522_ModWidthReg, 0x26))
        return ESP_ERR_INVALID_STATE;

    // Timer: starts automatically at end of transmission; 40 kHz tick
    // (TPrescaler 0x0A9) with reload 0x0FA0 -> ~100 ms timeout. (The
    // stock 25 ms value is too short for many cards to answer REQA.)
    if (!rc522_wr(RC522_TModeReg, 0x80) ||
        !rc522_wr(RC522_TPrescalerReg, 0xA9) ||
        !rc522_wr(RC522_TReloadRegHigh, 0x0F) ||
        !rc522_wr(RC522_TReloadRegLow, 0xA0))
        return ESP_ERR_INVALID_STATE;

    if (!rc522_wr(RC522_TxASKReg, RC522_FORCE_100_ASK))
        return ESP_ERR_INVALID_STATE;
    // CRC coprocessor preset value 0x6363 (ISO 14443-3 6.2.4).
    if (!rc522_wr(RC522_ModeReg, 0x3D))
        return ESP_ERR_INVALID_STATE;

    // Maximum receiver gain (RFCfgReg RxGain = 48 dB).
    if (!rc522_wr(0x26, 0x70))
        return ESP_ERR_INVALID_STATE;

    // Antenna stays OFF at idle: sessions (and nfcdump) enable the
    // field via rc522_field(), so a co-located writer — the rig's
    // ACR1252 reaching the sandwiched card — never sees two fields.
    if (!rc522_field(false))
        return ESP_ERR_INVALID_STATE;

    return ESP_OK;
}

bool rc522_probe(void)
{
    uint8_t ver;
    if (!s_dev || !rc522_rd(RC522_VersionReg, &ver))
        return false;
    return ver == 0x91 || ver == 0x92;
}

// Merge partial response bits into the anticollision buffer — mirrors
// FifoData::copy_bits_to from the Rust driver.
static void merge_rx_bits(uint8_t *dst, uint8_t dst_valid_bits,
                          const uint8_t *src, size_t src_bytes,
                          uint8_t src_last_bits)
{
    if (src_bytes == 0)
        return;
    size_t idx = dst_valid_bits / 8;
    uint8_t last_bits = dst_valid_bits % 8;
    uint8_t mask = 0xFF << last_bits;
    dst[idx] = (src[0] & mask) | (dst[idx] & ~mask);
    idx++;
    size_t rest = src_bytes - 1;
    if (rest > 0)
        memcpy(&dst[idx], &src[1], rest);
    (void)src_last_bits;
}

// SAK bit 0x04 clear => UID cascade complete.
static bool sak_is_complete(uint8_t sak) { return !(sak & 0x04); }

static bool reqa(uint8_t atqa[2])
{
    uint8_t tx = PICC_REQA;
    uint8_t rx[2];
    size_t rx_len;
    uint8_t rx_bits;
    if (!rc522_transceive(&tx, 1, 7, 0, rx, sizeof(rx), &rx_len, &rx_bits))
        return false;
    if (rx_len != 2 || rx_bits != 0)
        return false;
    atqa[0] = rx[0];
    atqa[1] = rx[1];
    return true;
}

bool rc522_poll(rc522_tag_t *out)
{
    // ReQA preamble per new_card_present(): restore framing registers
    // (they change during transceives).
    if (!rc522_wr(RC522_TxModeReg, 0x00) ||
        !rc522_wr(RC522_RxModeReg, 0x00) ||
        !rc522_wr(RC522_ModWidthReg, 0x26))
        return false;

    uint8_t atqa[2];
    if (!reqa(atqa)) {
        uint8_t wupa_tx = PICC_WUPA;
        uint8_t rx[2];
        size_t rx_len;
        uint8_t rx_bits;
        if (!rc522_transceive(&wupa_tx, 1, 7, 0, rx, sizeof(rx), &rx_len, &rx_bits)
            || rx_len != 2 || rx_bits != 0) {
            ESP_LOGD(TAG, "REQA/WUPA: no answer");
            return false;
        }
        atqa[0] = rx[0];
        atqa[1] = rx[1];
    }
    ESP_LOGI(TAG, "REQA: ATQA=%02X%02X", atqa[1], atqa[0]);

    // No ATQA tag-type-bit gating: NTAG424 (ATQA 0x4403) and other
    // multi-bit encodings still run standard Type A anticollision.

    // ValuesAfterColl off.
    uint8_t coll;
    if (!rc522_rd(RC522_CollReg, &coll)) {
        ESP_LOGD(TAG, "collreg read failed");
        return false;
    }
    if (!rc522_wr(RC522_CollReg, coll & 0x7F)) {
        ESP_LOGD(TAG, "collreg write failed");
        return false;
    }

    uint8_t uid_bytes[10] = { 0 };
    size_t uid_idx = 0;

    for (uint8_t cascade = 0; cascade < 3; cascade++) {
        uint8_t cmd = PICC_SEL_CL1 + cascade * 2;
        uint8_t known_bits = 0;
        uint8_t tx[9] = { 0 };
        tx[0] = cmd;
        ESP_LOGD(TAG, "cascade %d start", cascade);

        // Anticollision loop (bit-by-bit collision resolution).
        for (uint32_t guard = 0; guard < 32; guard++) {
            uint8_t tx_last_bits = known_bits % 8;
            uint8_t tx_bytes = 2 + known_bits / 8;
            size_t end = tx_bytes + (tx_last_bits ? 1 : 0);
            tx[1] = (uint8_t)((tx_bytes << 4) + tx_last_bits);

            uint8_t rx[5];
            size_t rx_len;
            uint8_t rx_bits;
            uint8_t coll_reg;
            bool ok = rc522_transceive(tx, end, tx_last_bits, tx_last_bits,
                                       rx, sizeof(rx), &rx_len, &rx_bits);
            if (!rc522_rd(RC522_CollReg, &coll_reg))
                return false;
            ESP_LOGD(TAG, "anticoll lvl=%d ok=%d rx_len=%d bits=%d coll=%02X known=%d",
                      cascade, ok, (int)rx_len, rx_bits, coll_reg, known_bits);

            if (ok) {
                merge_rx_bits(&tx[2], known_bits, rx, rx_len, rx_bits);
                break;
            }

            // Collision: CollPosNotValid or no-progress guard.
            if (coll_reg & (1 << 5))
                return false;
            uint8_t coll_pos = coll_reg & 0x1F;
            if (coll_pos == 0)
                coll_pos = 32;
            if (coll_pos < known_bits)
                return false;
            merge_rx_bits(&tx[2], known_bits, rx, rx_len, rx_bits);
            known_bits = coll_pos;

            uint8_t count = known_bits % 8;
            uint8_t check_bit = (known_bits - 1) % 8;
            size_t index = 1 + known_bits / 8 + (count ? 1 : 0);
            tx[index] |= 1 << check_bit;
        }

        // SELECT for this cascade level.
        tx[1] = 0x70;
        tx[6] = tx[2] ^ tx[3] ^ tx[4] ^ tx[5]; // BCC
        uint8_t crc[2];
        if (!rc522_calc_crc(tx, 7, crc)) {
            ESP_LOGD(TAG, "select lvl=%d crc-calc failed", cascade);
            return false;
        }
        tx[7] = crc[0];
        tx[8] = crc[1];

        uint8_t rx[3];
        size_t rx_len;
        uint8_t rx_bits;
        if (!rc522_transceive(tx, 9, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits)) {
            ESP_LOGD(TAG, "select lvl=%d transceive failed", cascade);
            return false;
        }
        if (rx_len != 3 || rx_bits != 0) {
            ESP_LOGD(TAG, "select lvl=%d bad frame rx_len=%d bits=%d",
                      cascade, (int)rx_len, rx_bits);
            return false;
        }

        uint8_t sak = rx[0];
        if (!rc522_calc_crc(rx, 1, crc) || crc[0] != rx[1] || crc[1] != rx[2]) {
            ESP_LOGD(TAG, "select lvl=%d CRC mismatch sak=%02X", cascade, sak);
            return false;
        }
        ESP_LOGD(TAG, "select lvl=%d sak=%02X", cascade, sak);

        if (!sak_is_complete(sak)) {
            memcpy(&uid_bytes[uid_idx], &tx[3], 3);
            uid_idx += 3;
        } else {
            memcpy(&uid_bytes[uid_idx], &tx[2], 4);
            uid_idx += 4;
            out->sak = sak;
            out->uid_len = uid_idx;
            memcpy(out->uid, uid_bytes, uid_idx);
            return true;
        }
    }
    return false;
}

bool rc522_ul_read(uint8_t page, uint8_t out[16])
{
    uint8_t tx[4] = { PICC_UL_READ, page, 0, 0 };
    uint8_t crc[2];
    if (!rc522_calc_crc(tx, 2, crc))
        return false;
    tx[2] = crc[0];
    tx[3] = crc[1];

    uint8_t rx[18];
    size_t rx_len;
    uint8_t rx_bits;
    if (!rc522_transceive(tx, 4, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits))
        return false;
    if (rx_len != 18 || rx_bits != 0)
        return false;
    if (!rc522_calc_crc(rx, 16, crc) || crc[0] != rx[16] || crc[1] != rx[17])
        return false;

    memcpy(out, rx, 16);
    return true;
}

// -------------------------------------------------------------------------
// ISO-DEP (ISO14443-4): CID-less T=CL for Type 4 NDEF tags
// -------------------------------------------------------------------------

bool rc522_field(bool on)
{
    uint8_t txctl;
    if (!rc522_rd(RC522_TxControlReg, &txctl))
        return false;
    return rc522_wr(RC522_TxControlReg, on ? (txctl | 0x03) : (txctl & ~0x03));
}

static void isodep_set_timeout_ms(uint32_t ms)
{
    // 40 kHz timer tick (prescaler 0xA9), reload capped to 16 bits.
    uint32_t reload = ms * 40000u / 1000u;
    if (reload > 0xFFFFu)
        reload = 0xFFFFu;
    rc522_wr(RC522_TModeReg, 0x80);
    rc522_wr(RC522_TPrescalerReg, 0xA9);
    rc522_wr(RC522_TReloadRegHigh, (reload >> 8) & 0xFF);
    rc522_wr(RC522_TReloadRegLow, reload & 0xFF);
}

bool rc522_isodep_connect(rc522_isodep_t *s)
{
    memset(s, 0, sizeof(*s));
    // Hardware CRC on both directions for T=CL frames.
    if (!rc522_wr(RC522_TxModeReg, 0x80) || !rc522_wr(RC522_RxModeReg, 0x80))
        return false;
    isodep_set_timeout_ms(20);

    // RATS: FSDI 6 (FSD 64), CID 0. Hardware CRC is appended/stripped.
    uint8_t rats[2] = {0xE0, 0x60};
    uint8_t rx[40];
    size_t rx_len;
    uint8_t rx_bits;
    if (!rc522_transceive(rats, 2, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits) ||
        rx_bits != 0 || rx_len < 2)
        goto fail;

    // ATS: TL [T0 [TA TB TC ...] historical]. FSCI (T0 low nibble)
    // sizes the card's frames; TB high nibble is the FWI.
    uint8_t tl = rx[0];
    if (tl < 2 || rx_len < tl)
        goto fail;
    uint8_t fsci = 8;
    uint32_t fwi = 4;
    if (tl > 1) {
        uint8_t t0 = rx[1];
        fsci = t0 & 0x0F;
        uint8_t n_if = (t0 >> 5) & 0x07;
        size_t pos = 2;
        for (uint8_t i = 0; i < n_if && pos < (size_t)tl && pos < rx_len; i++, pos++) {
            if (i == 1)
                fwi = rx[pos] >> 4; // TB
        }
    }
    s->fsc = fsci <= 8 ? (uint8_t)((uint16_t[]){16,24,32,40,48,64,96,128,256}[fsci]) : 64;
    if (s->fsc > 64)
        s->fsc = 64; // our FIFO
    uint32_t fwt_us = 302u << fwi;
    s->fwt_ms = (fwt_us + 999u) / 1000u;
    if (s->fwt_ms < 5)
        s->fwt_ms = 5;
    if (s->fwt_ms > 200)
        s->fwt_ms = 200;
    s->hw_crc = true;
    isodep_set_timeout_ms(s->fwt_ms);
    ESP_LOGI(TAG, "ISODEP up: ATS %d B, fsci=%d fsc=%d fwi=%u fwt=%lu ms",
             tl, fsci, s->fsc, (unsigned)fwi, (unsigned long)s->fwt_ms);
    return true;

fail:
    rc522_wr(RC522_TxModeReg, 0x00);
    rc522_wr(RC522_RxModeReg, 0x00);
    return false;
}

void rc522_isodep_end(rc522_isodep_t *s)
{
    rc522_wr(RC522_TxModeReg, 0x00);
    rc522_wr(RC522_RxModeReg, 0x00);
    isodep_set_timeout_ms(100);
    s->hw_crc = false;
}

bool rc522_isodep_exchange(rc522_isodep_t *s,
                           const uint8_t *apdu, size_t apdu_len,
                           uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (apdu_len + 3 > s->fsc)
        return false; // caller must chunk; we never chain our TX

    uint8_t tx[70];
    tx[0] = 0x02 | ((s->block_nr & 1) << 6); // I-block, no chaining
    memcpy(&tx[1], apdu, apdu_len);

    uint8_t rx[70];
    size_t rx_len;
    uint8_t rx_bits;
    if (!rc522_transceive(tx, apdu_len + 1, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits) ||
        rx_bits != 0)
        return false;
    s->block_nr ^= 1;

    size_t got = 0;
    for (;;) {
        if (rx_len < 1)
            return false;
        uint8_t pcb = rx[0];
        if ((pcb & 0x80) == 0) {
            // I-block from the card; collect INF, ACK chaining.
            size_t inf = rx_len - 1;
            if (got + inf > out_cap)
                return false;
            memcpy(&out[got], &rx[1], inf);
            got += inf;
            if (!(pcb & 0x10)) {
                *out_len = got;
                return true;
            }
            uint8_t ack = 0xA2 | (((pcb >> 6) & 1) ^ 1) << 6;
            if (!rc522_transceive(&ack, 1, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits) ||
                rx_bits != 0)
                return false;
        } else if (pcb == 0xF2 && rx_len >= 2) {
            // S(WTX): echo the multiplier, widen the window for the retry.
            uint8_t wtxm = rx[1] > 59 ? 59 : rx[1];
            isodep_set_timeout_ms(s->fwt_ms * (wtxm ? wtxm : 1));
            uint8_t reply[2] = {0xF2, wtxm};
            if (!rc522_transceive(reply, 2, 0, 0, rx, sizeof(rx), &rx_len, &rx_bits) ||
                rx_bits != 0)
                return false;
            isodep_set_timeout_ms(s->fwt_ms);
        } else {
            return false; // R-block or unexpected S-block mid-exchange
        }
    }
}
