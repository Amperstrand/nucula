// wallet_delta.hpp — H4 delta journal: bounded-append persistence engine
// (nucula issue #46 structural half; see docs/research/MCU-MONEY-SAFETY.md)
//
// Model: the wallet persists as CHECKPOINT (full NPB1 blob, existing
// A/B slots) + DELTA JOURNAL (a ring of small NVS keys, one written
// per receive — no whole-blob rewrite per payment, so per-save wear is
// bounded by the record size, not the wallet size).
//
// Boot: load checkpoint -> replay journal ring in seq order -> compact
// when the ring is over half full (checkpoint rewrite + ring erase).
// A torn tail (length overrun / bad seq) ends replay at the previous
// record — NVS per-key atomicity makes a single record all-or-nothing,
// so the only tear point is "last record absent", which is exactly the
// pre-journal state and safe to drop.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>

namespace cashu {

struct Proof;  // main/wallet_types.hpp

} // namespace cashu

namespace nucula {

// Injected key-value store: ESP binds NVS, host tests bind a std::map.
// Keys are small ("d000".."d031"); values are opaque byte strings.
struct DeltaKV {
    // returns false when the key does not exist
    std::function<bool(const std::string&, std::string&)> get;
    std::function<bool(const std::string&, const std::string&)> set;   // false = write failed
    std::function<bool(const std::string&)> erase;                     // false = erase failed
};

class DeltaJournal {
public:
    static constexpr int  RING_SLOTS   = 32;    // 32 records
    static constexpr size_t REC_MAX    = 512;   // bytes, payload cap
    static constexpr int  COMPACT_AT   = RING_SLOTS / 2; // compact when live > half

    explicit DeltaJournal(DeltaKV kv) : kv_(std::move(kv)) {}

    // Append one ADD-record of serialized new proofs (NPB1 fragment).
    // Fails (false) when the record exceeds REC_MAX or no slot is free
    // — caller must then run a full checkpoint (compaction) or refuse
    // the receive per the #45 ack semantics. Never throws.
    bool append(const std::string& proofs_bin);

    // Boot replay: returns records in seq order; stops at the first
    // torn/absent link (safe: the tail was never acked).
    // live_records = how many records survived (for compaction policy).
    std::vector<std::string> replay(int& live_records) const;

    // Ring state for policy: number of live records.
    int live() const;

    // Compaction step 1/2 after a successful checkpoint write:
    // erase every live ring key. Returns false only on erase failure
    // (harmless to retry; stale records are re-erased next compaction).
    bool clear();

    // Full engine status for logs/tests.
    int last_error() const { return last_err_; } // 0 ok, 1 rec-too-big, 2 ring-full, 3 kv-fail

private:
    DeltaKV kv_;
    int last_err_ = 0;
    int next_slot_ = -1;  // discovered during load(); -1 = not scanned

    static constexpr uint32_t REC_MAGIC = 0x4e504231; // "NPB1"
    // record layout: magic u32 | seq u32 | len u16 | payload[len]
    static std::string encode(uint32_t seq, const std::string& payload);
    // decodes a record value; false = torn/foreign
    static bool decode(const std::string& value, uint32_t& seq, std::string& payload);
    static std::string key_at(int i) { char k[8]; snprintf(k, sizeof(k), "d%03d", i); return k; }
};

} // namespace nucula
