// Regression tests: fresh-wallet boot must NOT degrade the wallet.
//
// Found live on the bench (2026-10-25, t-a7670g-tollgate E2E): a T-A7670G
// flashed with the TollGate PoC boots, loads keysets from the mint, then
// logs "proof load skipped/failed — wallet DEGRADED-READ-ONLY" — on a
// device whose NVS has NEVER held proofs. load_proofs() treated the
// ABSENT proofs blob like an unloadable one, so every first-boot wallet
// was read-only and could not receive its first token (the PoC's /pay
// answered "Payment failed" for a valid UNSPENT token).
//
// The fail-stop itself (PR #18 / issue #15 boundary work) stays: a blob
// that IS present but cannot be parsed still degrades the wallet. Only
// the "no persisted proofs, nothing to protect" case becomes clean.
//
// Uses the in-memory NVS shim; each test starts from an erased store
// (nvs_shim_reset()).

#include "shims/nvs.h"
#include "wallet.hpp"
#include "wallet_blob.hpp"
#include "wallet_internal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cashu {

// Provided by nvs_shim.cpp (host build); clears every namespace.
extern "C" void nvs_shim_reset();

namespace {

secp256k1_context *test_ctx()
{
    static secp256k1_context *ctx = nullptr;
    if (!ctx)
        ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

void put_raw_proof_blob(int slot, const void *data, size_t len)
{
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    char key[16];
    snprintf(key, sizeof(key), "proofs_%d", slot);
    nvs_set_blob(h, key, data, len);
    nvs_commit(h);
    nvs_close(h);
}

} // namespace

int fresh_wallet_run_tests()
{
    bool fresh_ok = true, corrupt_ok = true, roundtrip_ok = true;

    // 1. FRESH WALLET: no proofs key anywhere -> must boot writable.
    nvs_shim_reset();
    {
        Wallet w("https://mint.example", test_ctx(), 0);
        w.load_from_nvs();
        if (w.degraded()) {
            printf("  fresh store: degraded()=true (cannot ever receive a first token)\n");
            fresh_ok = false;
        }
    }

    // 2. FAIL-STOP PRESERVED: a present-but-garbage proofs blob still
    //    degrades the wallet (persisted state we cannot parse must be
    //    protected, not overwritten by an empty in-memory set).
    nvs_shim_reset();
    {
        const char garbage[] = "neither NPB1 magic nor JSON";
        put_raw_proof_blob(0, garbage, sizeof(garbage));
        Wallet w("https://mint.example", test_ctx(), 0);
        w.load_from_nvs();
        if (!w.degraded()) {
            printf("  garbage blob: degraded()=false (fail-stop regressed)\n");
            corrupt_ok = false;
        }
    }

    // 3. ROUND-TRIP: a REAL NPB1 blob (written the way save_proofs
    //    would) loads back clean and counts up. Pins that the fix does
    //    not mask a genuine load path failure behind the fresh case.
    nvs_shim_reset();
    {
        Proof p;
        p.id = std::string(32, 'a');
        p.amount = 8;
        p.secret = std::string(64, 'b');
        p.C = std::string(66, 'c');
        std::vector<Proof> one{p};
        size_t blen = 0;
        char *blob = cashu::proofs_to_bin_buf(one, &blen);
        if (!blob || blen == 0) {
            printf("  proofs_to_bin_buf failed\n");
            roundtrip_ok = false;
        } else {
            put_raw_proof_blob(0, blob, blen);
            free(blob);
            Wallet w("https://mint.example", test_ctx(), 0);
            w.load_from_nvs();
            if (w.degraded()) {
                printf("  real blob reload: degraded()=true\n");
                roundtrip_ok = false;
            }
            if (w.balance() != 8) {
                printf("  real blob reload: balance=%lld != 8\n",
                       (long long)w.balance());
                roundtrip_ok = false;
            }
        }
    }

    printf("[%s] fresh wallet: absent proofs blob boots writable (not degraded)\n",
           fresh_ok ? "PASS" : "FAIL");
    printf("[%s] fresh wallet: unparseable blob still degrades (fail-stop kept)\n",
           corrupt_ok ? "PASS" : "FAIL");
    printf("[%s] fresh wallet: save->reload round-trip stays writable\n",
           roundtrip_ok ? "PASS" : "FAIL");

    return (fresh_ok && corrupt_ok && roundtrip_ok) ? 0 : 1;
}

} // namespace cashu
