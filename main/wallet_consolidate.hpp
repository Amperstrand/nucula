// wallet_consolidate.hpp — H6 bounded wallet: consolidation policy
// (nucula issue #46; see docs/research/MCU-MONEY-SAFETY.md)
//
// The wallet stays under the persistence fail-stop bound BY
// CONSTRUCTION: when the proof set crosses a cap, a consolidation
// swap retires many small proofs for a few large ones (same value,
// mint-supported), shrinking both the serialized blob and future
// swap fan-in. Pure policy here — the swap itself runs through the
// existing Wallet::swap() (deterministic outputs + H1 write-ahead
// journal), and persistence flows through the H4 delta journal
// (ADD new proofs, REMOVE retired secrets).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nucula {

struct ConsolidationPolicy {
    // Trigger: proof count past which consolidation is worth a swap.
    // 64 proofs ~ 12-14 KB NPB1 — comfortably under the 32 KiB
    // fail-stop while leaving the ring room for receives.
    static constexpr size_t CAP_PROOFS   = 64;
    // Trigger: serialized size past 3/4 of the 32 KiB bound.
    static constexpr size_t CAP_BYTES    = 24 * 1024;
    // A consolidation must retire at least this many proofs to be
    // worth a mint round-trip + fee.
    static constexpr size_t MIN_RETIRE   = 8;
    // Never move more than this many inputs in one swap (fan-in cap;
    // matches the bench-measured happy path for the fakewallet mint).
    static constexpr size_t MAX_INPUTS   = 48;

    // true when the wallet should consolidate before the next receive
    // is acked (the #45 discipline: never let persistence overflow).
    static bool should_consolidate(size_t proof_count, size_t blob_bytes);

    // Selection: indices of the smallest-amount proofs (ties broken
    // deterministically by secret order at the call site) that reduce
    // proof count the most per value moved. Returns {} when fewer
    // than MIN_RETIRE proofs exist (a swap would not pay for itself).
    // `amounts` is the parallel amount view of the wallet's proofs.
    static std::vector<size_t> select_smallest(const std::vector<int64_t>& amounts);

    // Output plan for the retired total: fewest denominations using
    // the existing 2^k split (Wallet::split_amount semantics, kept
    // here as a pure mirror so host tests can verify sum preservation
    // and count reduction without the mint).
    static std::vector<int64_t> plan_outputs(int64_t total);
};

} // namespace nucula
