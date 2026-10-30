// wallet_delta.cpp — H4 delta journal engine (see wallet_delta.hpp).
// Pure C++ over the injected DeltaKV: host-testable, ESP binds NVS.
#include "wallet_delta.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace nucula {

// ---- record codec -------------------------------------------------

std::string DeltaJournal::encode(uint32_t seq, const std::string& payload)
{
    std::string v;
    v.reserve(10 + payload.size());
    uint32_t magic = REC_MAGIC;
    uint16_t len = static_cast<uint16_t>(payload.size());
    v.append(reinterpret_cast<const char*>(&magic), 4);
    v.append(reinterpret_cast<const char*>(&seq), 4);
    v.append(reinterpret_cast<const char*>(&len), 2);
    v.append(payload);
    return v;
}

bool DeltaJournal::decode(const std::string& value, uint32_t& seq, std::string& payload)
{
    if (value.size() < 10) return false;
    uint32_t magic; uint16_t len;
    memcpy(&magic, value.data(), 4);
    memcpy(&seq, value.data() + 4, 4);
    memcpy(&len, value.data() + 8, 2);
    if (magic != REC_MAGIC) return false;
    if ((size_t)len + 10 != value.size()) return false;  // torn tail
    payload.assign(value, 10, len);
    return true;
}

// ---- engine --------------------------------------------------------

int DeltaJournal::live() const
{
    // const-method scan (no caching): the ring is tiny, RING_SLOTS keys
    int n = 0;
    for (int i = 0; i < RING_SLOTS; ++i) {
        std::string v;
        if (kv_.get && kv_.get(key_at(i), v)) ++n;
    }
    return n;
}

bool DeltaJournal::append(const std::string& proofs_bin)
{
    last_err_ = 0;
    if (proofs_bin.size() > REC_MAX) { last_err_ = 1; return false; }

    // discover live records + highest seq (one scan; rings are small)
    uint32_t hi_seq = 0;
    int free_slot = -1, live_n = 0;
    std::vector<std::pair<uint32_t,int>> recs;  // seq -> slot
    for (int i = 0; i < RING_SLOTS; ++i) {
        std::string v;
        if (!kv_.get || !kv_.get(key_at(i), v)) { if (free_slot < 0) free_slot = i; continue; }
        uint32_t seq; std::string payload;
        if (!decode(v, seq, payload)) {
            // torn/foreign record: treat as free for the append, but
            // note replay stops there — compaction cleans it.
            if (free_slot < 0) free_slot = i;
            continue;
        }
        ++live_n;
        hi_seq = std::max(hi_seq, seq);
        recs.emplace_back(seq, i);
    }
    if (live_n >= RING_SLOTS) { last_err_ = 2; return false; }
    // append prefers the lowest free slot after a torn one; correctness
    // comes from seq order at replay, not slot order.
    if (free_slot < 0) free_slot = RING_SLOTS - 1;  // unreachable; belt+braces

    std::string rec = encode(hi_seq + 1, proofs_bin);
    if (!kv_.set || !kv_.set(key_at(free_slot), rec)) { last_err_ = 3; return false; }
    return true;
}

std::vector<std::string> DeltaJournal::replay(int& live_records) const
{
    live_records = 0;
    std::vector<std::pair<uint32_t,int>> recs;  // (seq, slot)
    for (int i = 0; i < RING_SLOTS; ++i) {
        std::string v;
        if (!kv_.get || !kv_.get(key_at(i), v)) continue;
        uint32_t seq; std::string payload;
        if (!decode(v, seq, payload)) continue;  // torn tail: stop at link level (seq gap handles order)
        recs.emplace_back(seq, i);
    }
    std::sort(recs.begin(), recs.end());
    // Chain from the END: walk backwards while seq decreases by exactly 1.
    // A gap can only come from a crash mid-clear() (per-record writes are
    // atomic), so the most-recent contiguous run is post-checkpoint state
    // and everything older is already inside the checkpoint — replaying it
    // would double-apply.
    std::vector<std::string> chain;
    if (!recs.empty()) {
        size_t i = recs.size();
        uint32_t want = recs.back().first;
        while (i > 0) {
            auto& [seq, slot] = recs[i - 1];
            if (seq != want) break;
            std::string v;
            kv_.get(key_at(slot), v);
            std::string payload;
            decode(v, seq, payload);
            chain.push_back(std::move(payload));
            --i; --want;
        }
        std::reverse(chain.begin(), chain.end());
    }
    live_records = (int)chain.size();
    return chain;
}

bool DeltaJournal::clear()
{
    bool ok = true;
    for (int i = 0; i < RING_SLOTS; ++i) {
        std::string v;
        if (!kv_.get || !kv_.get(key_at(i), v)) continue;
        if (kv_.erase && !kv_.erase(key_at(i))) ok = false;
    }
    return ok;
}

} // namespace nucula
