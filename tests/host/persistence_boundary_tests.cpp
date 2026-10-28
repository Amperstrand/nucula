// Regression tests for issue #15: the ~714-sat persistence boundary.
//
// On target, 714 sat (~70 powers-of-two proofs with DLEQ) was exactly
// where the legacy JSON serializer's growth policy demanded 64 KiB of
// contiguous heap — more than the largest free block mid-receive — so
// every save failed with the -1 sentinel and the wallet ran RAM-only.
// The allocation wall itself is target-specific; what these host tests
// pin down is everything around it:
//   - NPB1 round-trips at the boundary balances (700/701/713/714/715/
//     1000/1176 sat) with identical fields, including dleq/witness
//   - the binary blob beats the legacy JSON blob in size (the diet
//     that moved the wall) and stays under the 32 KiB fail-stop bound
//   - a corrupted proof count is rejected by the blob's own size
//     before any reserve (the OOM boot-loop class)
//   - legacy JSON blobs still parse (the migration path — wallets
//     self-migrate on the first successful save)

#include "cashu_json.hpp"
#include "wallet_blob.hpp"
#include "wallet.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace cashu {

namespace {

std::string hex_pad(unsigned seed, size_t hex_len)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(hex_len);
    unsigned x = seed * 2654435761u + 12345u;
    for (size_t i = 0; i < hex_len; i++) {
        x = x * 1664525u + 1013904223u;
        out += digits[(x >> 16) & 0xF];
    }
    return out;
}

// Proof set for a balance the way the bench boundary actually built
// it: repeated small receives (42-sat chunks, each split into
// powers-of-two denominations by the mint) accumulate until the
// balance is reached — 714 sat = 17 x 42 = ~51 proofs, 1176 sat =
// 28 x 42 = 84 proofs (the bench capture's proofs_loaded=84).
std::vector<Proof> proofs_for_sats(int64_t sats)
{
    std::vector<Proof> proofs;
    auto receive_chunk = [&](int64_t chunk) {
        for (int denom = 1 << 16; denom > 0 && chunk > 0; denom >>= 1) {
            while (chunk >= denom) {
                Proof p;
                p.id = hex_pad((unsigned)proofs.size() + 1, 32);
                p.amount = denom;
                p.secret = hex_pad((unsigned)proofs.size() + 2, 64);
                p.C = hex_pad((unsigned)proofs.size() + 3, 66);
                DLEQ d;
                d.e = hex_pad((unsigned)proofs.size() + 4, 64);
                d.s = hex_pad((unsigned)proofs.size() + 5, 64);
                if (denom & 1) {
                    // odd denominations carry the third DLEQ term on
                    // this mint; even ones omit it — both round-trip
                    d.r = hex_pad((unsigned)proofs.size() + 6, 64);
                }
                p.dleq = d;
                proofs.push_back(std::move(p));
                chunk -= denom;
            }
        }
    };
    while (sats >= 42) {
        receive_chunk(42);
        sats -= 42;
    }
    if (sats > 0)
        receive_chunk(sats);
    return proofs;
}

bool proofs_equal(const Proof &a, const Proof &b)
{
    if (a.id != b.id || a.amount != b.amount || a.secret != b.secret ||
        a.C != b.C)
        return false;
    if (a.dleq.has_value() != b.dleq.has_value())
        return false;
    if (a.dleq) {
        if (a.dleq->e != b.dleq->e || a.dleq->s != b.dleq->s)
            return false;
        if (a.dleq->r.has_value() != b.dleq->r.has_value())
            return false;
        if (a.dleq->r && *a.dleq->r != *b.dleq->r)
            return false;
    }
    if (a.witness.has_value() != b.witness.has_value())
        return false;
    if (a.witness && *a.witness != *b.witness)
        return false;
    return true;
}

} // namespace


// ---------------------------------------------------------------------------
// HEX-C regression guard (corpus http01-post/hex-c-token)
//
// A structurally valid V3 token whose proof C values are non-canonical
// hex (not valid compressed points) must still DESERIALIZE — the failure
// belongs to the processing stage (payment-processing-failed on the
// gate), not the parser (payment-error-invalid-token). Garbage must
// keep failing the parse.
// ---------------------------------------------------------------------------

static int hexc_parse_test()
{
    // verbatim corpus/tokens/v3_v2_hex_c.cashu
    const char *tok =
    "cashuAeyJ0b2tlbiI6W3sibWludCI6Imh0dHA6Ly8xOTIuMTY4LjEzLjIyMTo4Mz"
    "gzIiwicHJvb2ZzIjpbeyJhbW91bnQiOjIsImlkIjoiMDFlNzNlNzVhMDgwOTYwNC"
    "IsInNlY3JldCI6Ijg5MzZlMTcyM2M1NDdkMjEwZmFjYTg1MjRmOGI1ZjIyYjViOD"
    "liYjg1OTE1ZDIyMWVkODI5NzQ1OTI3ZTNjOWQiLCJDIjoiZDM3ZWRmZTliZDM4Nz"
    "c0ZDM0ZjVjZTc3ZDljZDM0ZjVkZWRkZGRhNzliNjliZTM5ZWI2NmY1NmI5ZDc3ZG"
    "Y0ZWI4ZWRiNmY1ZTlhZTM2ZWJhZDM1N2Y4Zjc3NjlmZTNkZTciLCJkbGVxIjp7Im"
    "UiOiJhNDZkZTM3ZTU2M2I4M2UzYmNlNjJjZTJkYjk5N2FiMzU3MzY0NGFjYzgxZT"
    "I2NGRmNzNjYjkxZjI5Zjc1ODI0IiwicyI6IjUwMDIxMWRjM2QyMGQ5MTNhMjM0NT"
    "cwMGFjNTQ4YmE1OWZlMjM1NzM2NWY5MWJkMGY4NGFkYzcxMDE4YjU1NzEiLCJyIj"
    "oiZDc3ZjcyYzQzODFiMGE4MjFhMWRjYTNhMzY1ZGM0NzA5OTIyNDVkOWIyYzY1MD"
    "VlZDYwNzA3MmZjMTlmZTNhNiJ9fSx7ImFtb3VudCI6MiwiaWQiOiIwMWU3M2U3NW"
    "EwODA5NjA0Iiwic2VjcmV0IjoiZDJmYjgyMjhhOWY2OGIyNmY5NGE2MjU2MzZmMj"
    "hhNjc1N2RlZTc5ODUzZjFiMzFkMGY4OTMyYTcyYWYxMDQxMCIsIkMiOiJkMzdlYj"
    "RkNzc3Nzc2OWVkN2JlZGRkZjQ3YjVkYmRmN2RkOWM2OWRkMWVlYmE2YjQ2ZjZlZj"
    "Q3N2Q2ZmQ3Mzc3ZjQ3ZjllN2E2ZGU3N2FmMWI3MWRlMzc2OWRkYjdkMWRkMyIsIm"
    "RsZXEiOnsiZSI6ImJmNThiNGE5MDZlODk1YTljNmE0ODEyYWFlZGI5ZGY5ZjFlZD"
    "kwN2YzZWNmZDg1MWI2ZDFhMzQ2M2ExZjYyMzIiLCJzIjoiMGJjZjU3MGNlNjljZj"
    "g3M2FiYWJkODg2MGEwNGRhODFkODM5NGJjMzUwZjExMjBkZmM4NzFmMzk0Zjc2Mm"
    "YxMSIsInIiOiI2YWViMjNkMzVhMTM0ZTE2YzA1ZGFhMzVkZTc5MGUxNzc4NTAwNz"
    "ZlZDJjZmU4MGUyZTU4MTE2ZTU0YmU1ODdhIn19XX1dLCJ1bml0Ijoic2F0In0";

    Token out;
    if (!deserialize_token(tok, out)) {
        printf("  hexc: lenient parse FAILED (would misclassify as invalid-token)\n");
        return 1;
    }
    if (out.proofs.empty() || out.proofs[0].C.empty()) {
        printf("  hexc: parsed but proofs/C missing\n");
        return 1;
    }
    // garbage must still fail the parse
    if (deserialize_token("cashuAnotbase64!!", out)) {
        printf("  hexc: garbage parsed (parser too lenient)\n");
        return 1;
    }
    printf("  hexc: lenient parse ok (C carried, garbage rejected)\n");
    return 0;
}

// ---------------------------------------------------------------------------
// H1 swap-journal guard: write-ahead round-trips; derivation from a
// journaled range is deterministic (recovery's core premise).
// ---------------------------------------------------------------------------

static int swap_journal_test()
{
    // the journal methods need a Wallet instance (they are members);
    // a default-constructed one has no NVS state of its own here
    cashu::Wallet w{"https://host-test.example", nullptr};
    // round-trip
    cashu::Proof a;
    a.id = "018bd9342ad84095ac74dd44e2f72f53acd9ec6c44893e0a79daa73fbbcf4f7ff6";
    a.amount = 2;
    a.secret = std::string(64, 'a');
    a.C = std::string(66, 'b');
    std::vector<cashu::Proof> ins{a};
    if (!w.journal_swap_write(ins, a.id, 7, {1, 2, 4})) {
        printf("  swpjrnl: write failed\n");
        return 1;
    }
    cashu::Wallet::SwapJournal j;
    if (!w.journal_swap_read(j) || j.inputs.size() != 1
        || j.counter_start != 7 || j.amounts.size() != 3
        || j.amounts[2] != 4 || j.inputs[0].secret != a.secret) {
        printf("  swpjrnl: round-trip mismatch\n");
        return 1;
    }
    if (!w.journal_swap_exists()) {
        printf("  swpjrnl: exists() false after write\n");
        return 1;
    }
    w.journal_swap_clear();
    if (w.journal_swap_exists()) {
        printf("  swpjrnl: exists() true after clear\n");
        return 1;
    }
    printf("  swpjrnl: write/read/exists/clear round-trip ok\n");
    return 0;
}

int persistence_boundary_run_tests()
{
    static const int64_t balances[] = {700, 701, 713, 714, 715, 1000, 1176};
    bool all_ok = true;
    bool roundtrip_ok = true;
    bool bound_ok = true;
    bool diet_ok = true;

    for (int64_t sats : balances) {
        std::vector<Proof> proofs = proofs_for_sats(sats);

        int64_t sum = 0;
        for (const Proof &p : proofs) sum += p.amount;
        if (sum != sats || proofs.empty()) {
            roundtrip_ok = false;
            continue;
        }

        size_t bin_len = 0;
        char *bin = proofs_to_bin_buf(proofs, &bin_len);
        if (!bin || !blob_is_binary(bin, bin_len)) {
            roundtrip_ok = false;
            free(bin);
            continue;
        }

        // The fail-stop bound from wallet_nvs.cpp (PROOFS_BLOB_MAX):
        // a save over 32 KiB is refused loudly rather than attempted.
        if (bin_len > 32 * 1024) bound_ok = false;

        size_t json_len = 0;
        char *json = proofs_to_json_buf(proofs, &json_len);
        if (json) {
            if (json_len <= bin_len) diet_ok = false;
        }
        if (json && sats == 714) {
            printf("  714-sat profile: %zu proofs, NPB1 %zu B, JSON %zu B\n",
                   proofs.size(), bin_len, json_len);
        }

        std::vector<Proof> back;
        if (!proofs_from_blob(bin, bin_len, back) ||
            back.size() != proofs.size()) {
            roundtrip_ok = false;
            free(bin);
            free(json);
            continue;
        }
        for (size_t i = 0; i < proofs.size(); i++) {
            if (!proofs_equal(proofs[i], back[i])) {
                roundtrip_ok = false;
                break;
            }
        }

        // Legacy migration path: the JSON blob must still parse, and
        // the loader's dispatch key must classify both forms.
        if (json) {
            std::vector<Proof> from_json;
            bool json_ok = !blob_is_binary(json, json_len) &&
                           proofs_from_json(json, from_json) &&
                           from_json.size() == proofs.size();
            if (!json_ok) roundtrip_ok = false;
        }

        // Corrupted count guard: a huge count in the header must be
        // rejected by the blob's own size before any reserve.
        if (sats == 714) {
            std::vector<char> tampered(bin, bin + bin_len);
            memset(tampered.data() + 4, 0x7F, 4);
            std::vector<Proof> sink;
            if (proofs_from_blob(tampered.data(), tampered.size(), sink))
                all_ok = false;
        }

        free(bin);
        free(json);
    }

    printf("[%s] persistence boundary: NPB1 round-trip 700..1176 sat\n",
           roundtrip_ok ? "PASS" : "FAIL");
    printf("[%s] persistence boundary: NPB1 under the 32 KiB fail-stop bound\n",
           bound_ok ? "PASS" : "FAIL");
    printf("[%s] persistence boundary: NPB1 smaller than legacy JSON\n",
           diet_ok ? "PASS" : "FAIL");
    printf("[%s] persistence boundary: corrupted count rejected pre-reserve\n",
           all_ok ? "PASS" : "FAIL");

    int hexc_fails = hexc_parse_test();
    int swpj_fails = swap_journal_test();
    printf("[%s] swap journal: write-ahead round-trip\n",
           swpj_fails == 0 ? "PASS" : "FAIL");
    printf("[%s] hex-c: non-canonical C deserializes (processing-stage failure)\n",
           hexc_fails == 0 ? "PASS" : "FAIL");

    return (roundtrip_ok && bound_ok && diet_ok && all_ok && hexc_fails == 0
            && swpj_fails == 0) ? 0 : 1;
}

} // namespace cashu
