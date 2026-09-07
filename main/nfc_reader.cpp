// NFC reader-mode frontend for the M5Stack Atom + MFRC522 board.
//
// Mirrors the PN7160 card-emulation frontend (nfc.cpp) behind the same
// nfc.hpp API: the user starts a payment session, and instead of waiting
// for a reader to tap our emulated tag, we wait for a Type 2 NDEF tag
// (an ACR1252 in MIFARE Ultralight card-emulation mode, or a real
// NTAG/Ultralight sticker) carrying a cashuA/cashuB token.

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_ATOM

#include "nfc.hpp"
#include "task_config.h"
#include "board.h"
#include "ndef.hpp"
#include "nfc_common.h"
#include "wallet.hpp"
#include "wallet_store.hpp"
#include "unit.hpp"
#include "ui.h"
#include "wifi.h"
#include "http.h"
#include "rc522.h"

#include <cstring>
#include <string>
#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "nfc"

#define PAYMENT_TIMEOUT_MS 120000
#define POLL_INTERVAL_MS   200

static bool s_hw_init = false;
static std::atomic<NfcState> s_state{NfcState::off};
static std::atomic<bool> s_stop_flag{false};
static TaskHandle_t s_task_handle = nullptr;
// Given by nfc_task right before it deletes itself; nfc_request_stop
// blocks on it instead of polling s_task_handle.
static SemaphoreHandle_t s_task_done = nullptr;

// -------------------------------------------------------------------------
// Type 2 NDEF tag reading
// -------------------------------------------------------------------------

namespace {

// Incremental Type 2 memory reader: one 4-page (16-byte) READ in the
// cache at a time, refilled on demand as the TLV walk advances.
class T2Memory {
public:
    // Fetch one byte at absolute byte address `addr`.
    bool byte_at(uint32_t addr, uint8_t &out)
    {
        uint8_t page = addr / 4;
        if (!have_page(page)) {
            if (!rc522_ul_read(page, buf_))
                return false;
            cached_page_ = page;
        }
        out = buf_[addr % 4];
        return true;
    }

private:
    bool have_page(uint8_t page) const
    {
        return cached_page_ >= 0 && page >= cached_page_ && page < cached_page_ + 4;
    }

    uint8_t buf_[16];
    int cached_page_ = -1;
};

} // namespace

// Reads the NDEF message TLV from a Type 2 tag and returns the first
// Text/URI record's content via the shared NDEF parser.
static bool t2_read_ndef_text(std::string &text_out)
{
    T2Memory mem;

    // Capability Container: 4 bytes at page 3 (byte address 12).
    uint8_t cc[4];
    for (int i = 0; i < 4; i++) {
        if (!mem.byte_at(12 + i, cc[i]))
            return false;
    }
    if (cc[0] != 0xE1) {
        ESP_LOGD(TAG, "t2: no NDEF CC magic (%02X)", cc[0]);
        return false;
    }

    // TLV area starts at page 4. Walk TLVs until the NDEF message TLV.
    static uint8_t msg[NDEF_MAX_DATA_SIZE];
    uint32_t addr = 16;
    for (;;) {
        if (addr > 16 + NDEF_MAX_DATA_SIZE)
            return false; // malformed TLV area
        uint8_t t;
        if (!mem.byte_at(addr, t))
            return false;
        if (t == 0x00) { addr++; continue; }   // padding
        if (t == 0xFE) return false;           // terminator, no NDEF
        if (t != 0x03) return false;           // not NDEF — bail

        uint32_t len;
        uint8_t l0;
        if (!mem.byte_at(addr + 1, l0))
            return false;
        if (l0 == 0xFF) {
            uint8_t hi, lo;
            if (!mem.byte_at(addr + 2, hi) || !mem.byte_at(addr + 3, lo))
                return false;
            len = ((uint32_t)hi << 8) | lo;
            addr += 4;
        } else {
            len = l0;
            addr += 2;
        }
        if (len == 0 || len > NDEF_MAX_DATA_SIZE)
            return false;

        for (uint32_t i = 0; i < len; i++) {
            if (!mem.byte_at(addr + i, msg[i]))
                return false;
        }
        // ndef_parse_message takes the Type 4 FILE form (NLEN + records);
        // a Type 2 TLV carries bare records, so stage them behind an
        // NLEN prefix in the same buffer.
        if (len + 2 > NDEF_MAX_DATA_SIZE)
            return false;
        memmove(&msg[2], msg, len);
        msg[0] = (uint8_t)(len >> 8);
        msg[1] = (uint8_t)len;
        return ndef_parse_message(msg, len + 2, text_out);
    }
}

// Type 4 NDEF (ISO-DEP): SELECT the NDEF application and files, read
// the message via READ BINARY. Mirrors the NFC Forum Type 4 Tag
// mapping; every APDU response carries SW 9000 after the data.
static bool t4t_read_ndef_text(std::string &text_out)
{
    rc522_isodep_t s;
    if (!rc522_isodep_connect(&s)) {
        ESP_LOGW(TAG, "t4: RATS/ATS failed");
        return false;
    }
    bool ok = false;
    uint8_t r[70];
    size_t rl;

    auto ok_sw = [&]() { return rl >= 2 && r[rl - 2] == 0x90 && r[rl - 1] == 0x00; };

    do {
        static const uint8_t SEL_APP[] = {
            0x00, 0xA4, 0x04, 0x00, 0x07,
            0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00,
        };
        if (!rc522_isodep_exchange(&s, SEL_APP, sizeof(SEL_APP), r, sizeof(r), &rl) ||
            !ok_sw()) {
            ESP_LOGW(TAG, "t4: NDEF app select failed");
            break;
        }

        static const uint8_t SEL_CC[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
        if (!rc522_isodep_exchange(&s, SEL_CC, sizeof(SEL_CC), r, sizeof(r), &rl) ||
            !ok_sw() || rl < 17) { // 15 CC bytes + SW
            ESP_LOGW(TAG, "t4: CC select failed");
            break;
        }
        // CC layouts vary by mapping version: v1.x carries the NDEF
        // FID at bytes 7-8; v2.0 cards (this NTAG424: len 0x17, ver
        // 0x20) list a size field there and the FID at 9-10. The NFC
        // Forum well-known E104 works on both — try it first, fall
        // back to the CC bytes 7-8.
        uint16_t ndef_fid = 0xE104;
        uint8_t sel_file[] = {0x00, 0xA4, 0x00, 0x0C, 0x02,
                              (uint8_t)(ndef_fid >> 8), (uint8_t)ndef_fid};
        if (!rc522_isodep_exchange(&s, sel_file, sizeof(sel_file), r, sizeof(r), &rl) ||
            !ok_sw()) {
            ndef_fid = ((uint16_t)r[7] << 8) | r[8];
            sel_file[5] = (uint8_t)(ndef_fid >> 8);
            sel_file[6] = (uint8_t)ndef_fid;
            if (!rc522_isodep_exchange(&s, sel_file, sizeof(sel_file), r, sizeof(r), &rl) ||
                !ok_sw()) {
                ESP_LOGW(TAG, "t4: NDEF file select failed");
                break;
            }
        }

        static const uint8_t RD_NLEN[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
        if (!rc522_isodep_exchange(&s, RD_NLEN, sizeof(RD_NLEN), r, sizeof(r), &rl) ||
            !ok_sw() || rl < 4) {
            ESP_LOGW(TAG, "t4: NLEN read failed");
            break;
        }
        uint16_t nlen = ((uint16_t)r[0] << 8) | r[1];
        if (nlen == 0 || nlen > NDEF_MAX_DATA_SIZE) {
            ESP_LOGW(TAG, "t4: bad NLEN %u", (unsigned)nlen);
            break;
        }

        // Read the whole NDEF file (NLEN first) so the parser gets the
        // file form it expects.
        if ((size_t)nlen + 2 > NDEF_MAX_DATA_SIZE)
            break;
        static uint8_t msg[NDEF_MAX_DATA_SIZE];
        size_t total = (size_t)nlen + 2;
        size_t got = 0;
        bool read_ok = true;
        while (got < total) {
            uint8_t chunk = (uint8_t)(total - got < 32 ? total - got : 32);
            uint8_t rd[] = {0x00, 0xB0, (uint8_t)(got >> 8),
                            (uint8_t)got, chunk};
            if (!rc522_isodep_exchange(&s, rd, sizeof(rd), r, sizeof(r), &rl) ||
                !ok_sw() || rl < (size_t)chunk + 2) {
                ESP_LOGW(TAG, "t4: read @%u failed", (unsigned)got);
                read_ok = false;
                break;
            }
            memcpy(&msg[got], r, chunk);
            got += chunk;
        }
        if (!read_ok)
            break;

        ok = ndef_parse_message(msg, total, text_out);
    } while (false);

    rc522_isodep_end(&s);
    return ok;
}

// -------------------------------------------------------------------------
// Reader session task
// -------------------------------------------------------------------------

struct NfcRequestParams {
    int amount;
    std::string unit;
    std::string mint_url;
};

static void nfc_task(void *arg)
{
    auto *params = static_cast<NfcRequestParams *>(arg);

    rc522_field(true); // radiate only while a session is active
    vTaskDelay(pdMS_TO_TICKS(20)); // tag power-up from a cold field

    // Reader mode receives pre-minted tokens; there is no NUT-18 request
    // to encode. Prime TLS to the expected mint while waiting anyway.
    if (wifi_is_connected()) {
        if (!params->mint_url.empty()) {
            http_prewarm(params->mint_url.c_str());
        } else {
            wallet_store_guard guard;
            for (int i = 0; i < MAX_MINTS; i++) {
                auto *w = wallet_store_get(i);
                if (w) { http_prewarm(w->mint_url().c_str()); break; }
            }
        }
    }

    int64_t start = esp_timer_get_time();
    char amt_str[24];
    cashu::format_amount(amt_str, sizeof(amt_str), params->amount,
                         params->unit.c_str());

    s_state.store(NfcState::waiting);
    ui_show_nfc_status("waiting for tag", amt_str);

    while (!s_stop_flag.load()) {
        if ((esp_timer_get_time() - start) > (int64_t)PAYMENT_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "payment timeout");
            s_state.store(NfcState::error);
            ui_show_nfc_status("nfc timeout", "");
            break;
        }

        rc522_tag_t tag;
        if (!rc522_poll(&tag)) {
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            continue;
        }

        ESP_LOGI(TAG, "tag detected (uid_len=%d sak=%02X)",
                 (int)tag.uid_len, tag.sak);
        s_state.store(NfcState::active);

        std::string text;
        bool have_text = false;
        if (tag.sak & 0x20) {
            have_text = t4t_read_ndef_text(text);
            if (!have_text) {
                ESP_LOGI(TAG, "t4 failed, falling back to t2 read");
                vTaskDelay(pdMS_TO_TICKS(10)); // settle between T4T and T2T reads
                have_text = t2_read_ndef_text(text);
            }
        } else {
            have_text = t2_read_ndef_text(text);
        }
        if (!have_text) {
            ESP_LOGW(TAG, "no NDEF text on tag");
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_state.store(NfcState::waiting);
            continue;
        }

        std::string token = ndef_extract_cashu_token(text);
        if (token.empty()) {
            ESP_LOGW(TAG, "no cashu token in: %.60s", text.c_str());
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_state.store(NfcState::waiting);
            continue;
        }

        ESP_LOGI(TAG, "cashu token (%d chars)", (int)token.size());
        s_state.store(NfcState::redeeming);
        ui_show_nfc_status("redeeming...", amt_str);

        char recv_str[32];
        snprintf(recv_str, sizeof(recv_str), "%s", amt_str);
        int rc = nfc_redeem_or_stash(token, params->unit.c_str(),
                                     recv_str, sizeof(recv_str));
        if (rc == 1) {
            s_state.store(NfcState::success);
            ui_show_nfc_status("paid!", recv_str);
            ui_refresh();
        } else if (rc == 0) {
            s_state.store(NfcState::success);
            ui_show_nfc_status("queued", recv_str);
            ui_refresh();
        } else {
            s_state.store(NfcState::error);
            ui_show_nfc_status("redeem failed", "");
        }
        break;
    }

    rc522_field(false);
    delete params;
    s_task_handle = nullptr;
    xSemaphoreGive(s_task_done);
    vTaskDelete(nullptr);
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

bool nfc_init(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        s_state.store(NfcState::off);
        return false;
    }

    int retries = 3;
    while (retries-- > 0) {
        esp_err_t err = rc522_init(bus, BOARD_MFRC522_ADDR);
        if (err == ESP_OK && rc522_probe()) {
            s_hw_init = true;
            s_state.store(NfcState::idle);
            ESP_LOGI(TAG, "MFRC522 ready");
            return true;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            // Chip present but unresponsive; a retry may recover it.
        }
        ESP_LOGW(TAG, "MFRC522 setup failed, retrying... (%d left)", retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "MFRC522 unavailable, NFC disabled");
    s_state.store(NfcState::off);
    return false;
}

bool nfc_request_start(int amount, const char *unit, const char *mint_url)
{
    if (!s_hw_init) return false;
    if (s_task_handle) { ESP_LOGW(TAG, "already running"); return false; }

    if (!s_task_done)
        s_task_done = xSemaphoreCreateBinary();
    if (!s_task_done)
        return false;
    // Drain a give left by a task that finished on its own (success or
    // timeout without a stop), so the next stop can't return early.
    xSemaphoreTake(s_task_done, 0);

    auto *p = new NfcRequestParams();
    p->amount = amount;
    p->unit = (unit && unit[0]) ? unit : cashu::Wallet::default_unit();
    if (mint_url) p->mint_url = mint_url;

    s_stop_flag.store(false);
    if (xTaskCreate(nfc_task, "nfc", NUCULA_TASK_STACK_NFC, p,
                    NUCULA_TASK_PRIO_NFC, &s_task_handle) != pdPASS) {
        delete p;
        return false;
    }
    return true;
}

void nfc_request_stop()
{
    s_stop_flag.store(true);
    // The task gives s_task_done just before deleting itself; a stuck task
    // is abandoned after 5 s, but the common case returns as soon as the
    // task is actually gone instead of on a 100 ms poll grid.
    if (s_task_handle && s_task_done &&
        xSemaphoreTake(s_task_done, pdMS_TO_TICKS(5000)) != pdTRUE)
        ESP_LOGW(TAG, "nfc task did not stop within 5s");
    s_task_handle = nullptr;
    s_state.store(s_hw_init ? NfcState::idle : NfcState::off);
}

NfcState nfc_state() { return s_state.load(); }

const char *nfc_status_str()
{
    switch (s_state.load()) {
        case NfcState::off:       return "off";
        case NfcState::idle:      return "idle";
        case NfcState::waiting:   return "waiting";
        case NfcState::active:    return "active";
        case NfcState::redeeming: return "redeeming";
        case NfcState::success:   return "success";
        case NfcState::error:     return "error";
    }
    return "?";
}

#endif // CONFIG_NUCULA_BOARD_ATOM
