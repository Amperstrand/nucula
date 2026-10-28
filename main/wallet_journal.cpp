// Swap journal — crash-recovery write-ahead for money operations
// (research/mcu-money-safety H1; ecosystem pattern: cdk's PaymentAttempted
// write-ahead, minibits' reservations counter-range, CocoCashu's
// CounterRepository "persist the advance before reserve returns").
//
// Written BEFORE the swap POST; cleared only after the resulting proofs
// are durably saved (or the mint gave a definitive negative answer, which
// proves the inputs were not spent by us). Recovery re-derives the
// blinded outputs from the journaled counter range + seed, re-POSTs the
// same request (a cdk-family mint's NUT-19-style replay cache returns the
// original signatures — cashu-audit COMPAT-LEDGER), unblinds, adopts,
// saves, clears.
//
// Ambiguity rule: transport failure (no HTTP answer) keeps the journal —
// the swap may or may not have executed at the mint. A definitive mint
// answer clears it: a rejection means the inputs were not spent; a
// success is handled by the caller's save-then-clear.
#include "wallet.hpp"
#include "wallet_internal.hpp"
#include "cashu_json.hpp"
#include "nvs.h"
#include "crypto.h"
#include "cashu_suite.h"
#include "keyset.hpp"
#include "hex.h"
#include <cstdio>
#include <cstring>
#include <esp_log.h>

namespace cashu {

static const char* JTAG = "swapjournal";
static const char* JOURNAL_KEY = "swpjrnl";

// ---- tiny local cJSON helpers (avoid depending on get_int etc.) ----
static long jnum(const cJSON* j, const char* k, long dflt)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsNumber(v) ? (long)v->valueint : dflt;
}

static std::string jstr(const cJSON* j, const char* k)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
}

bool Wallet::journal_swap_write(const std::vector<Proof>& inputs,
                                const std::string& keyset_id,
                                uint32_t counter_start,
                                const std::vector<int>& amounts)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "keyset", keyset_id.c_str());
    cJSON_AddNumberToObject(root, "c0", (double)counter_start);
    cJSON* am = cJSON_CreateArray();
    for (int a : amounts) cJSON_AddItemToArray(am, cJSON_CreateNumber(a));
    cJSON_AddItemToObject(root, "amounts", am);
    cJSON* ins = cJSON_CreateArray();
    for (const auto& p : inputs) cJSON_AddItemToArray(ins, to_json(p));
    cJSON_AddItemToObject(root, "inputs", ins);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return false;

    Nvs nvs(NVS_READWRITE);
    if (!nvs.ok()) { cJSON_free(s); return false; }
    esp_err_t err = nvs_set_blob(nvs.get(), JOURNAL_KEY, s, strlen(s));
    if (err == ESP_OK) err = nvs_commit(nvs.get());
    cJSON_free(s);
    if (err != ESP_OK) {
        ESP_LOGE(JTAG, "journal write failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(JTAG, "journal: %d inputs, counter %lu+%d (keyset %.8s…)",
             (int)inputs.size(), (unsigned long)counter_start,
             (int)amounts.size(), keyset_id.c_str());
    return true;
}

bool Wallet::journal_swap_read(SwapJournal& out)
{
    Nvs nvs(NVS_READONLY);
    if (!nvs.ok()) return false;
    static char buf[2048];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(nvs.get(), JOURNAL_KEY, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGE(JTAG, "journal read: %s", esp_err_to_name(err));
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(buf, len);
    if (!root) { ESP_LOGE(JTAG, "journal unparseable (len %u)", (unsigned)len); return false; }
    out.keyset_id = jstr(root, "keyset");
    out.counter_start = (uint32_t)jnum(root, "c0", 0);
    const cJSON* am = cJSON_GetObjectItemCaseSensitive(root, "amounts");
    out.amounts.clear();
    if (cJSON_IsArray(am))
        for (const cJSON* a = am->child; a; a = a->next)
            if (cJSON_IsNumber(a)) out.amounts.push_back((int)a->valueint);
    const cJSON* ins = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    out.inputs.clear();
    bool ok = true;
    if (cJSON_IsArray(ins)) {
        for (const cJSON* p = ins->child; p; p = p->next) {
            Proof pr;
            if (!from_json(p, pr)) { ok = false; break; }
            out.inputs.push_back(std::move(pr));
        }
    } else ok = false;
    cJSON_Delete(root);
    return ok && !out.inputs.empty() && !out.amounts.empty() && !out.keyset_id.empty();
}

bool Wallet::journal_swap_clear()
{
    Nvs nvs(NVS_READWRITE);
    if (!nvs.ok()) return false;
    esp_err_t err = nvs_erase_key(nvs.get(), JOURNAL_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err == ESP_OK) err = nvs_commit(nvs.get());
    return err == ESP_OK;
}

bool Wallet::journal_swap_exists()
{
    SwapJournal j;
    return journal_swap_read(j);
}

// Derive the journaled outputs again (amount-independent secrets from
// seed+keyset+counter; B_ depends only on secret+r, but amounts are
// journaled so the re-POST is byte-identical and the replay cache
// answers with the ORIGINAL signatures — unblindable with these rs).
bool Wallet::journal_derive_outputs(const SwapJournal& j, BlindingData& out)
{
    if (!s_seed_loaded) return false;
    const cashu_suite_t* suite = suite_for_id(j.keyset_id);
    if (!suite) return false;

    out.outputs.clear(); out.secrets.clear(); out.blinding_factors.clear();
    for (size_t i = 0; i < j.amounts.size(); i++) {
        unsigned char secret_bytes[32], r_bytes[32];
        if (!suite->derive_secret(s_seed, 64, j.keyset_id.c_str(),
                                  j.counter_start + (uint32_t)i, secret_bytes))
            return false;
        if (!suite->derive_r(s_seed, 64, j.keyset_id.c_str(),
                             j.counter_start + (uint32_t)i, r_bytes))
            return false;
        char secret_hex[65]; bytes_to_hex(secret_bytes, 32, secret_hex);
        char r_hex[65]; bytes_to_hex(r_bytes, 32, r_hex);
        unsigned char B[CASHU_MAX_POINT_LEN]; size_t B_len = sizeof(B);
        if (!suite->blind((void*)ctx_,
                          (const unsigned char*)secret_hex, strlen(secret_hex),
                          r_bytes, 32, B, &B_len))
            return false;
        char B_hex[CASHU_MAX_POINT_LEN * 2 + 1]; bytes_to_hex(B, B_len, B_hex);
        BlindedMessage m;
        m.amount = j.amounts[i];
        m.B_ = std::string(B_hex);
        m.id = j.keyset_id;
        out.outputs.push_back(std::move(m));
        out.secrets.emplace_back(secret_hex);
        out.blinding_factors.emplace_back(r_hex);
    }
    return true;
}

} // namespace cashu
