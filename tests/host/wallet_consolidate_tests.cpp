// wallet_consolidate_tests.cpp — H6 policy units (host, no mint).
// Covers: trigger thresholds, selection (smallest-first, caps, min
// count), output planning (sum preservation, 2^k denominations).
#include "wallet_consolidate.hpp"

#include <cstdio>
#include <numeric>
#include <vector>

using nucula::ConsolidationPolicy;

namespace cashu {

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int run_consolidation_tests()
{
    printf("[ consolidation policy (H6) ]\n");

    // 1. trigger thresholds
    {
        CHECK(!ConsolidationPolicy::should_consolidate(10, 4096));   // small+small
        CHECK(ConsolidationPolicy::should_consolidate(65, 1024));   // count past cap
        CHECK(ConsolidationPolicy::should_consolidate(10, 24 * 1024 + 1)); // bytes past cap
        CHECK(!ConsolidationPolicy::should_consolidate(64, 24 * 1024));    // exactly at both caps
    }

    // 2. selection: smallest-first, deterministic ties, fan-in cap
    {
        std::vector<int64_t> amt = {8, 1, 64, 2, 1, 4, 32, 2, 16, 1};
        // pad to past MIN_RETIRE so selection is allowed
        for (int i = 0; i < 10; ++i) amt.push_back(64);
        auto sel = ConsolidationPolicy::select_smallest(amt);
        CHECK(sel.size() == 20);
        // every selected amount <= every unselected amount
        int64_t max_sel = 0, min_unsel = 1 << 30;
        std::vector<bool> taken(amt.size(), false);
        for (size_t i : sel) { taken[i] = true; if (amt[i] > max_sel) max_sel = amt[i]; }
        for (size_t i = 0; i < amt.size(); ++i)
            if (!taken[i] && amt[i] < min_unsel) min_unsel = amt[i];
        CHECK(max_sel <= min_unsel);
        // ascending order (stable => deterministic for ties)
        for (size_t i = 1; i < sel.size(); ++i) CHECK(amt[sel[i-1]] <= amt[sel[i]]);
    }

    // 3. selection guards: below MIN_RETIRE -> empty
    {
        std::vector<int64_t> amt(7, 1);   // 7 < 8
        CHECK(ConsolidationPolicy::select_smallest(amt).empty());
    }

    // 4. fan-in cap: 100 proofs -> exactly MAX_INPUTS selected
    {
        std::vector<int64_t> amt(100, 1);
        auto sel = ConsolidationPolicy::select_smallest(amt);
        CHECK(sel.size() == ConsolidationPolicy::MAX_INPUTS);
    }

    // 5. plan_outputs: sum preserved, 2^k denominations, largest first
    {
        for (int64_t total : {1LL, 2LL, 3LL, 21LL, 100LL, 1092LL, 1000000LL}) {
            auto out = ConsolidationPolicy::plan_outputs(total);
            int64_t sum = std::accumulate(out.begin(), out.end(), 0LL);
            CHECK(sum == total);
            CHECK(out.size() <= 32);
            for (size_t i = 1; i < out.size(); ++i) CHECK(out[i-1] >= out[i]); // sorted desc
            for (int64_t v : out) { // power of two
                CHECK(v > 0 && (v & (v - 1)) == 0);
            }
            // count reduction property: binary decomposition is compact
            if (total == 1092) CHECK(out.size() == 3);   // verified exactly below
        }
        auto out = ConsolidationPolicy::plan_outputs(1092);
        CHECK(out.size() == 3);   // 1024 + 64 + 4
        CHECK(out[0] == 1024 && out[1] == 64 && out[2] == 4);
    }

    printf(failures ? "  %d FAILURES\n" : "  ok\n", failures);
    return failures;
}

} // namespace cashu
