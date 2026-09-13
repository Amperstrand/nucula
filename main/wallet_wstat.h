#pragma once
// Synchronous wallet-persistence diagnostics. Async log lines are
// structurally unreliable on the bench bridge (its RX buffer drops
// unread bytes between command windows), so save/load outcomes are also
// recorded here and reported on demand via the `wstat` console command.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t saves;         // successful save_proofs commits
    uint32_t save_fails;    // failed save_proofs (any branch)
    bool last_save_ok;
    int last_err;           // esp_err_t of the last failing nvs call, 0 otherwise
    size_t last_blob;       // bytes of the last serialized proofs blob
    int loaded_proofs;      // proof count of the last successful load
    uint32_t load_fails;    // failed load_proofs
} wallet_nvs_stat_t;

extern wallet_nvs_stat_t g_wallet_nvs_stat;

#ifdef __cplusplus
}
#endif
