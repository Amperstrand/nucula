// wallet_consolidate.cpp — H6 policy implementation (pure, host-tested).
#include "wallet_consolidate.hpp"

#include <algorithm>
#include <numeric>

namespace nucula {

bool ConsolidationPolicy::should_consolidate(size_t proof_count, size_t blob_bytes)
{
    return proof_count > CAP_PROOFS || blob_bytes > CAP_BYTES;
}

std::vector<size_t> ConsolidationPolicy::select_smallest(const std::vector<int64_t>& amounts)
{
    if (amounts.size() < MIN_RETIRE) return {};

    std::vector<size_t> idx(amounts.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t a, size_t b) { return amounts[a] < amounts[b]; });

    size_t take = std::min(amounts.size(), MAX_INPUTS);
    idx.resize(take);
    return idx;  // already smallest-first (stable: deterministic ties)
}

std::vector<int64_t> ConsolidationPolicy::plan_outputs(int64_t total)
{
    // Mirror of Wallet::split_amount (2^k greedy), 64-bit safe.
    std::vector<int64_t> out;
    if (total <= 0) return out;
    for (int64_t v = 1; total > 0 && out.size() < 32; v <<= 1) {
        if (total & v) { out.push_back(v); total -= v; }
        if (v > (int64_t(1) << 40)) break;  // belt: no runaway
    }
    std::reverse(out.begin(), out.end());  // largest first like split_amount
    return out;
}

} // namespace nucula
