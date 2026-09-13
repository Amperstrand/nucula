#include "nfc_common.h"

#include "cashu.hpp"
#include "cashu_json.hpp"
#include "wallet.hpp"
#include "wallet_store.hpp"
#include "unit.hpp"
#include "wifi.h"

#include <esp_log.h>

#define TAG "nfc"

int nfc_redeem_or_stash(const std::string &token_str,
                        const char *expected_unit,
                        char *desc, size_t desc_len)
{
    // Hold the store for the whole redeem/stash: the wallet pointer must
    // stay valid across the swap, and stickup/mint-remove must not
    // interleave with an in-flight redemption.
    wallet_store_guard guard;

    cashu::Token token;
    if (!cashu::deserialize_token(token_str.c_str(), token)) {
        ESP_LOGE(TAG, "token decode failed");
        return -1;
    }

    long long input_total = cashu::proofs_sum(token.proofs);
    char amt[32];
    cashu::format_amount(amt, sizeof(amt), input_total, token.unit.c_str());
    ESP_LOGI(TAG, "token: %s, %d proofs from %s",
             amt, (int)token.proofs.size(), token.mint.c_str());
    if (desc && desc_len)
        snprintf(desc, desc_len, "%s", amt);

    if (expected_unit && expected_unit[0] && token.unit != expected_unit)
        ESP_LOGW(TAG, "payer sent %s, request asked for %s — accepting",
                 token.unit.c_str(), expected_unit);

    if (!wifi_is_connected()) {
        // Offline: only accept a token from a mint we already hold keysets
        // for. Verifying the proofs' DLEQ (NUT-12) requires that mint's
        // public keys; without them a forged token is indistinguishable
        // from real ecash, and there is no online swap to fall back on.
        // Refuse — and do NOT create a new mint slot for an unknown mint.
        cashu::Wallet *w = wallet_store_find(token.mint.c_str());
        if (!w || w->keysets().empty()) {
            ESP_LOGE(TAG, "offline: refusing token from unknown mint %s "
                          "(no keysets, cannot verify DLEQ)",
                     token.mint.c_str());
            return -1;
        }
        if (!w->stash_pending_token(token_str)) {
            ESP_LOGE(TAG, "pending: stash failed");
            return -1;
        }
        ESP_LOGI(TAG, "offline: stashed %s for later drain", amt);
        return 0;
    }

    cashu::Wallet *w = wallet_store_get_or_create(token.mint);
    if (!w) return -1;

    if (w->keysets().empty() || !w->active_keyset(token.unit)) {
        if (!w->load_keysets()) { ESP_LOGE(TAG, "keyset load failed"); return -1; }
    }

    std::vector<cashu::Proof> received;
    if (!w->receive(token, received)) { ESP_LOGE(TAG, "swap failed"); return -1; }

    cashu::format_amount(amt, sizeof(amt),
                         cashu::proofs_sum(received), token.unit.c_str());
    if (desc && desc_len)
        snprintf(desc, desc_len, "%s", amt);
    ESP_LOGI(TAG, "redeemed %s (%d proofs)", amt, (int)received.size());
    return 1;
}
