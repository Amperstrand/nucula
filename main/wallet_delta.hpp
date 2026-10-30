// wallet_delta.hpp — H4 delta journal: bounded-append persistence engine
// + H6 REMOVE records (consolidation retires inputs, not just adds).
// (nucula issue #46; see docs/research/MCU-MONEY-SAFETY.md)
//
// Model: the wallet persists as CHECKPOINT (full NPB1 blob, existing
// A/B slots) + DELTA JOURNAL (a ring of small NVS keys, one written
// per receive/consolidation — no whole-blob rewrite per payment, so
// per-save wear is bounded by the record size, not the wallet size).
//
// Records: ADD (NPB1 proofs fragment — a receive) or REMOVE (packed
// 32-byte secrets — inputs retired by a consolidation swap).
//
// Boot: load checkpoint -> replay journal ring in seq order, applying
// ADDs and REMOVEs -> compact when the ring is over half full
// (checkpoint rewrite + ring erase). A torn tail (length overrun /
// bad seq) ends replay at the previous record — NVS per-key atomicity
// makes a single record all-or-nothing, so the only tear point is
// "last record absent", which is exactly the pre-journal state and
// safe to drop.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>

namespace cashu {

struct Proof;  // main/cashu.hpp

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

enum class DeltaOp : uint8_t { Add = 0, Remove = 1 };

struct DeltaRec {
    DeltaOp op;
    std::string payload;
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

    // Append one REMOVE-record: packed raw 32-byte secrets of the
    // proofs a consolidation swap retired. Same failure semantics.
    bool append_remove(const std::string& raw_secrets_bin);

    // Boot replay: returns records in seq order; stops at the first
    // torn/absent link (safe: the tail was never acked).
    // live_records = how many records survived (for compaction policy).
    std::vector<DeltaRec> replay(int& live_records) const;

    // Ring state for policy: number of live records.
    int live() const;

    // Compaction step 1/2 after a successful checkpoint write:
    // erase every live ring key. Returns false only on erase failure
    // (harmless to retry; stale records are re-erased next compaction).
    bool clear();

    // Full engine status for logs/tests.
    int last_error() const { return last_err_; } // 0 ok, 1 rec-too-big, 2 ring-full, 3 kv-fail

private:
    bool append_op(DeltaOp op, const std::string& payload);

    DeltaKV kv_;
    int last_err_ = 0;
    int next_slot_ = -1;  // discovered during load(); -1 = not scanned

    static constexpr uint32_t REC_MAGIC = 0x4e504232; // "NPB2" (op byte added)
    // record layout: magic u32 | seq u32 | op u8 | len u16 | payload[len]
    static std::string encode(uint32_t seq, DeltaOp op, const std::string& payload);
    // decodes a record value; false = torn/foreign
    static bool decode(const std::string& value, uint32_t& seq, DeltaOp& op, std::string& payload);
    static std::string key_at(int i) { char k[8]; snprintf(k, sizeof(k), "d%03d", i); return k; }
};

} // namespace nucula
