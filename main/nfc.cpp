#include "sdkconfig.h"

#if !CONFIG_NUCULA_BOARD_ATOM

#include "nfc.hpp"
#include "task_config.h"
#include "ndef.hpp"
#include "nfc_common.h"
#include "cashu.hpp"
#include "cashu_json.hpp"
#include "cashu_cbor.hpp"
#include "wallet.hpp"
#include "wallet_store.hpp"
#include "unit.hpp"
#include "ui.h"
#include "wifi.h"
#include "http.h"

#include <cstring>
#include <string>
#include <atomic>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "nci.h"

#define TAG "nfc"

#define PAYMENT_TIMEOUT_MS 120000

static nci_context_t s_nci;
static bool s_hw_init = false;
static std::atomic<NfcState> s_state{NfcState::off};
static std::atomic<bool> s_stop_flag{false};
static TaskHandle_t s_task_handle = nullptr;
// Given by nfc_task right before it deletes itself; nfc_request_stop
// blocks on it instead of polling s_task_handle.
static SemaphoreHandle_t s_task_done = nullptr;

// -------------------------------------------------------------------------
// NDEF write callback
// -------------------------------------------------------------------------

// Single-task hand-off: on_ndef_written runs synchronously inside
// ndef_handle_apdu on the nfc task itself, so no synchronization is
// needed — the flag is only ever read after the call that may set it.
static struct {
    bool received;
    std::string token;
} s_rx;

static void on_ndef_written(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "NDEF written (%d bytes)", (int)len);

    std::string text;
    if (!ndef_parse_message(data, len, text)) {
        ESP_LOGW(TAG, "NDEF parse failed");
        return;
    }

    std::string token = ndef_extract_cashu_token(text);
    if (token.empty()) {
        ESP_LOGW(TAG, "no cashu token in: %.60s", text.c_str());
        return;
    }

    ESP_LOGI(TAG, "cashu token (%d chars)", (int)token.size());
    s_rx.token = std::move(token);
    s_rx.received = true;
}

// -------------------------------------------------------------------------
// NFC task
// -------------------------------------------------------------------------

struct NfcRequestParams {
    int amount;
    std::string unit;
    std::string mint_url;
};

static void nfc_task(void *arg)
{
    auto *params = static_cast<NfcRequestParams *>(arg);

    // Build payment request and load into NDEF layer. NUT-18: `u` MUST be
    // set when an amount is set.
    cashu::PaymentRequest req;
    req.amount     = params->amount;
    req.unit       = params->unit;
    req.single_use = true;
    if (!params->mint_url.empty())
        req.mints = std::vector<std::string>{params->mint_url};

    // Offline-receive: ask the sender to lock the proofs to our P2PK pubkey
    // so we can swap them once WiFi returns. Online we skip the lock for
    // privacy (single static key would link receives).
    if (!wifi_is_connected() && cashu::Wallet::ensure_p2pk_keypair(wallet_store_ctx())) {
        cashu::NUT10Option opt;
        opt.kind = "P2PK";
        opt.data = cashu::Wallet::p2pk_pubkey_hex();
        req.nut10 = std::move(opt);
        ESP_LOGI(TAG, "offline: requesting P2PK lock to %s",
                 cashu::Wallet::p2pk_pubkey_hex());
    }

    std::string creq = cashu::serialize_payment_request(req);
    ESP_LOGI(TAG, "creq: %s", creq.c_str());

    if (creq.empty() || !ndef_set_message(creq.c_str())) {
        ESP_LOGE(TAG, "payment request encode/NDEF failed");
        s_state.store(NfcState::error);
        ui_show_nfc_status("nfc error", "request too large");
        delete params;
        s_task_handle = nullptr;
        xSemaphoreGive(s_task_done);
        vTaskDelete(nullptr);
        return;
    }

    ndef_set_receive_callback(on_ndef_written);

    // Full radio responsiveness while a payment is in flight; restored on
    // every exit path below.
    wifi_set_low_latency(true);

    // Prime the TLS connection to the expected mint while the user is
    // still tapping: the post-tap swap then reuses it.
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
    ui_show_nfc_status("tap to pay", amt_str);

    while (!s_stop_flag.load()) {
        // Timeout check
        if ((esp_timer_get_time() - start) > (int64_t)PAYMENT_TIMEOUT_MS * 1000) {
            ESP_LOGW(TAG, "payment timeout");
            s_state.store(NfcState::error);
            ui_show_nfc_status("nfc timeout", "");
            break;
        }

        // Poll IRQ with 100 ms window so we can also check stop flag / timeout
        if (!nci_poll_frame(&s_nci, 100)) continue;

        const uint8_t *frame = nci_frame(&s_nci);
        uint32_t frame_len = nci_frame_len(&s_nci);
        uint8_t mt  = frame[0];
        uint8_t oid = frame[1];

        // CORE_CONN_CREDITS_NTF — flow control, ignore
        if (mt == (NCI_MT_NTF | NCI_GID_CORE) &&
            oid == NCI_OID_CORE_CONN_CREDITS) continue;

        // Reader detected
        if (mt == (NCI_MT_NTF | NCI_GID_RF) &&
            oid == NCI_RF_INTF_ACTIVATED_NTF) {
            ESP_LOGI(TAG, "reader detected");
            s_state.store(NfcState::active);
            ndef_reset_receive();
            continue;
        }

        // Reader removed
        if (mt == (NCI_MT_NTF | NCI_GID_RF) && oid == NCI_RF_DEACTIVATE_NTF) {
            uint8_t dtype = frame_len > 3 ? frame[3] : 0xFF;
            ESP_LOGI(TAG, "reader removed (type=%d)", dtype);
            if (dtype != 3 && nci_restart_discovery(&s_nci) != ESP_OK)
                ESP_LOGW(TAG, "restart discovery failed");
            ndef_reset_receive();
            if (!s_rx.received) {
                s_state.store(NfcState::waiting);
                ui_show_nfc_status("tap to pay", amt_str);
            }
            continue;
        }

        // DATA packet (conn 0) — APDU from reader
        if (mt == NCI_MT_DATA && oid == 0x00) {
            uint8_t apdu_len = frame[2];
            static uint8_t rsp_buf[NCI_MAX_FRAME_SIZE];
            size_t rsp_len = 0;

            ndef_handle_apdu(&frame[3], apdu_len, rsp_buf, &rsp_len);
            if (nci_send_data(&s_nci, rsp_buf, rsp_len) != ESP_OK)
                ESP_LOGW(TAG, "APDU response write failed");

            // Check if a token arrived via the callback
            if (s_rx.received) {
                s_state.store(NfcState::redeeming);
                ui_show_nfc_status("redeeming...", amt_str);

                // Show what was actually received (unit may differ from
                // the request); fall back to the requested amount.
                char recv_str[32];
                snprintf(recv_str, sizeof(recv_str), "%s", amt_str);
                int rc = nfc_redeem_or_stash(s_rx.token,
                                               params->unit.c_str(),
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
            continue;
        }

        // Discovery RSP / NTF after restart, core error NTFs — ignore
        if ((mt == (NCI_MT_RSP | NCI_GID_RF) && oid == NCI_OID_RF_DISCOVER) ||
            (mt == (NCI_MT_NTF | NCI_GID_RF) && oid == NCI_RF_DISCOVER_NTF) ||
            (mt == (NCI_MT_NTF | NCI_GID_CORE) &&
             (oid == NCI_OID_CORE_GENERIC_ERROR ||
              oid == NCI_OID_CORE_INTERFACE_ERROR))) continue;

        ESP_LOGD(TAG, "unhandled NCI: %02X %02X", mt, oid);
    }

    wifi_set_low_latency(false);
    ndef_set_receive_callback(nullptr);
    ndef_clear_message();
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
    ndef_init();
    memset(&s_nci, 0, sizeof(s_nci));

    int retries = 3;
    while (retries-- > 0) {
        esp_err_t err = nci_setup_cardemu(&s_nci, bus);
        if (err == ESP_OK) {
            s_hw_init = true;
            s_state.store(NfcState::idle);
            ESP_LOGI(TAG, "PN7160 ready");
            return true;
        }
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG) {
            // Chip absent (or no bus) — retrying won't make it appear.
            break;
        }
        ESP_LOGW(TAG, "NCI setup failed, retrying... (%d left)", retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "PN7160 unavailable, NFC disabled");
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
    s_rx.received = false;
    s_rx.token.clear();
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
    // is abandoned after 5 s as before, but the common case returns as
    // soon as the task is actually gone instead of on a 100 ms poll grid.
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

#endif // !CONFIG_NUCULA_BOARD_ATOM
