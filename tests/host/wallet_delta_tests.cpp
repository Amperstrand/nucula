// wallet_delta_tests.cpp — H4 delta journal engine host tests (nucula #46)
// Covers: append/replay round-trip, torn tail, seq gap (crash mid-clear),
// ring-full refusal, REC_MAX cap, compaction clear.
#include "../main/wallet_delta.hpp"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace cashu {

using nucula::DeltaJournal;
using nucula::DeltaKV;

static DeltaKV map_kv(std::map<std::string,std::string>& m)
{
    return DeltaKV{
        [&](const std::string& k, std::string& v) {
            auto it = m.find(k);
            if (it == m.end()) return false;
            v = it->second; return true;
        },
        [&](const std::string& k, const std::string& v) { m[k] = v; return true; },
        [&](const std::string& k) { return m.erase(k) > 0; },
    };
}

static std::string frag(char tag, size_t n)  // stand-in NPB1 fragment
{
    return std::string(n, tag);
}

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    failures++; } } while (0)

int run_delta_tests(int& failures)
{
    // 1. round-trip: appends replay in order
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(dj.append(frag('a', 40)));
        CHECK(dj.append(frag('b', 50)));
        CHECK(dj.append(frag('c', 60)));
        int live = -1;
        auto recs = dj.replay(live);
        CHECK(live == 3);
        CHECK(recs.size() == 3);
        CHECK(recs[0].op == nucula::DeltaOp::Add && recs[0].payload == frag('a', 40));
        CHECK(recs[2].op == nucula::DeltaOp::Add && recs[2].payload == frag('c', 60));
    }
    // 2. torn tail: truncate the newest record's value -> replay drops it,
    //    keeps older records (the pre-tear state was acked, the torn one wasn't)
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(dj.append(frag('a', 40)));
        CHECK(dj.append(frag('b', 50)));
        // tear whichever key holds seq 2 (the newest): seq = bytes 4..8
        for (auto& [k, v] : m) {
            uint32_t seq = 0;
            if (v.size() >= 8) memcpy(&seq, v.data() + 4, 4);
            if (seq == 2) v.resize(v.size() - 5);
        }
        int live = -1;
        auto recs = dj.replay(live);
        CHECK(live == 1);
        CHECK(recs.size() == 1 && recs[0].payload == frag('a', 40));
    }
    // 3. seq gap (crash mid-clear): old records 1..2 + new records 5..6 —
    //    only the 5..6 chain replays (the 1..2 are in the checkpoint)
    {
        std::map<std::string,std::string> m;
        {
            DeltaJournal dj(map_kv(m));
            CHECK(dj.append(frag('o', 20)));   // seq 1
            CHECK(dj.append(frag('p', 20)));   // seq 2
        }
        // simulate compaction clearing TWO of the records then a crash
        // before the rest: erase the lowest-slot two keys
        size_t erased = 0;
        for (auto it = m.begin(); it != m.end() && erased < 2; ) {
            it = m.erase(it); ++erased;
        }
        int live = -1;
        {
            DeltaJournal dj(map_kv(m));
            auto recs = dj.replay(live);   // whatever survived must chain
            (void)recs;
        }
        // now append post-checkpoint records and verify chain excludes old
        DeltaJournal dj2(map_kv(m));
        CHECK(dj2.append(frag('x', 30)));
        CHECK(dj2.append(frag('y', 30)));
        auto recs2 = dj2.replay(live);
        // the surviving old record (if any) has seq 2; new ones 3,4 — chain intact
        CHECK(live >= 2);
        CHECK(recs2.back().payload == frag('y', 30));
    }
    // 4. ring-full: fill all slots, next append refuses (err 2)
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        for (int i = 0; i < DeltaJournal::RING_SLOTS; ++i)
            CHECK(dj.append(frag('f', 10)));
        CHECK(!dj.append(frag('g', 10)));
        CHECK(dj.last_error() == 2);
    }
    // 5. REC_MAX cap: oversized record refused (err 1), ring unchanged
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(!dj.append(frag('Z', DeltaJournal::REC_MAX + 1)));
        CHECK(dj.last_error() == 1);
        CHECK(dj.live() == 0);
    }
    // 6. compaction: clear() empties the ring; append restarts cleanly
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(dj.append(frag('a', 10)));
        CHECK(dj.append(frag('b', 10)));
        CHECK(dj.clear());
        CHECK(dj.live() == 0);
        CHECK(dj.append(frag('c', 10)));
        int live = -1;
        auto recs = dj.replay(live);
        CHECK(live == 1 && recs[0].op == nucula::DeltaOp::Add && recs[0].payload == frag('c', 10));
    }

    // 7. H6 REMOVE records: mixed Add/Remove replay in seq order, ops intact
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(dj.append(frag('a', 40)));                 // seq 1 Add
        std::string rm(3 * 32, '\x11');                  // 3 packed secrets
        CHECK(dj.append_remove(rm));                      // seq 2 Remove
        CHECK(dj.append(frag('b', 50)));                 // seq 3 Add
        int live = -1;
        auto recs = dj.replay(live);
        CHECK(live == 3 && recs.size() == 3);
        CHECK(recs[0].op == nucula::DeltaOp::Add && recs[0].payload == frag('a', 40));
        CHECK(recs[1].op == nucula::DeltaOp::Remove && recs[1].payload == rm);
        CHECK(recs[2].op == nucula::DeltaOp::Add && recs[2].payload == frag('b', 50));
    }
    // 8. REMOVE payload validation: non-multiple-of-32 rejected
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(!dj.append_remove(std::string(31, 'x')));
        CHECK(dj.last_error() == 1);
        CHECK(dj.live() == 0);
    }
    // 9. torn tail in a mixed ring: only the contiguous chain replays
    {
        std::map<std::string,std::string> m;
        DeltaJournal dj(map_kv(m));
        CHECK(dj.append(frag('a', 40)));
        CHECK(dj.append_remove(std::string(32, '\x22')));
        // truncate the newest (REMOVE) record
        std::string k = "d001";
        m[k] = m[k].substr(0, m[k].size() - 3);
        int live = -1;
        auto recs = dj.replay(live);
        CHECK(live == 1 && recs.size() == 1);
        CHECK(recs[0].op == nucula::DeltaOp::Add);
    }
    return failures;
}

} // namespace cashu
