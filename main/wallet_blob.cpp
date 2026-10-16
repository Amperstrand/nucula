// Binary persistence blob — the compact representation that keeps the
// wallet savable at proof counts where the JSON form structurally
// cannot be (fork issue #15: ~70 proofs serialized to ~28 KiB of JSON;
// even tight-growth allocation exceeded the largest free heap block
// mid-receive). Hex strings decode to bytes (halving), fields are
// length-prefixed, DLEQ and witness are optional-presence flags.
//
// Layout ("NPB1" = nucula proofs binary v1):
//   magic "NPB1" | u32 count | per proof:
//     u16 id_len | id bytes
//     i32 amount
//     u16 secret_len | secret bytes
//     u16 C_len | C bytes
//     u8 has_dleq | (e[32] s[32] r_flag u8 + r[32]?)   — hex strings are
//       fixed 64-char scalars (32 B); C is a 66-char point (33 B); ids
//       are 32-char (16 B). Length prefixes stay for safety.
//     u16 witness_len | witness bytes
//
// The loader dispatches on the magic; JSON blobs (legacy) keep loading,
// and the first successful save rewrites the wallet in binary form.

#include "wallet_blob.hpp"

#include <cstring>
#include "esp_log.h"
#include "hex.h"

namespace cashu {

static const char BLOB_MAGIC[4] = {'N', 'P', 'B', '1'};
#define TAG "wallet_blob"

struct BinWriter {
    char *p = nullptr;
    size_t n = 0, cap = 0;
    bool oom = false;
    bool owned = true;
    BinWriter() = default;
    BinWriter(char *ext, size_t extcap) : p(ext), cap(extcap), owned(false) {}
    void need(size_t extra) {
        if (oom || n + extra <= cap) return;
        if (!owned) { oom = true; return; }
        size_t nc = n + extra + (n + extra) / 8 + 256;
        char *q = static_cast<char *>(realloc(p, nc));
        if (!q) { oom = true; return; }
        p = q;
        cap = nc;
    }
    void raw(const void *d, size_t l) { need(l); if (!oom) { memcpy(p + n, d, l); n += l; } }
    void u8(unsigned char v) { raw(&v, 1); }
    void u16(unsigned v) { unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)}; raw(b, 2); }
    void i32(int v) { unsigned u = static_cast<unsigned>(v); unsigned char b[4] = {static_cast<unsigned char>(u), static_cast<unsigned char>(u >> 8), static_cast<unsigned char>(u >> 16), static_cast<unsigned char>(u >> 24)}; raw(b, 4); }
    void u32(unsigned v) { unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)}; raw(b, 4); }
    // Fixed-length hex field (length implied by the byte count).
    void hex_fixed(const std::string &h, size_t bytes) {
        unsigned char buf[64];
        if (h.size() != bytes * 2 || bytes > sizeof(buf) || hex_to_bytes(h.c_str(), buf, bytes) != 1) {
            oom = true; // caller aborts on malformed proof
            return;
        }
        raw(buf, bytes);
    }
    // Variable-length hex field, length-prefixed in BYTES.
    void hex_var(const std::string &h) {
        size_t bytes = h.size() / 2;
        if (h.size() % 2 != 0 || bytes > 0xFFFF) { oom = true; return; }
        unsigned char buf[512];
        if (bytes > sizeof(buf) || (bytes && hex_to_bytes(h.c_str(), buf, bytes) != 1)) { oom = true; return; }
        u16(unsigned(bytes));
        raw(buf, bytes);
    }
};

struct BinReader {
    const char *p = nullptr;
    size_t n = 0, cap = 0;
    bool bad = false;
    bool take(size_t l, void *out) {
        if (bad || n + l > cap) { bad = true; return false; }
        memcpy(out, p + n, l);
        n += l;
        return true;
    }
    unsigned char u8v() { unsigned char v = 0; take(1, &v); return v; }
    unsigned u16v() { unsigned char b[2] = {0, 0}; take(2, b); return static_cast<unsigned>(b[0]) | static_cast<unsigned>(b[1]) << 8; }
    int i32v() { unsigned char b[4] = {0, 0, 0, 0}; take(4, b); return static_cast<int>(static_cast<unsigned>(b[0]) | static_cast<unsigned>(b[1]) << 8 | static_cast<unsigned>(b[2]) << 16 | static_cast<unsigned>(b[3]) << 24); }
    unsigned u32v() { unsigned char b[4] = {0, 0, 0, 0}; take(4, b); return static_cast<unsigned>(b[0]) | static_cast<unsigned>(b[1]) << 8 | static_cast<unsigned>(b[2]) << 16 | static_cast<unsigned>(b[3]) << 24; }
    static std::string hex_str(const unsigned char *d, size_t l) {
        std::string s;
        s.resize(l * 2 + 1);
        bytes_to_hex(d, l, s.data());
        s.resize(l * 2);
        return s;
    }
    std::string hex_fixed(size_t bytes) {
        unsigned char buf[64];
        if (bytes > sizeof(buf) || !take(bytes, buf)) return {};
        return hex_str(buf, bytes);
    }
    std::string hex_var() {
        unsigned bytes = u16v();
        unsigned char buf[512];
        if (bytes > sizeof(buf) || !take(bytes, buf)) return {};
        return hex_str(buf, bytes);
    }
};

bool blob_is_binary(const char *data, size_t len)
{
    return len >= 4 && memcmp(data, BLOB_MAGIC, 4) == 0;
}

char *proofs_to_bin_buf(const std::vector<Proof> &proofs, size_t *len)
{
    BinWriter w;
    w.raw(BLOB_MAGIC, 4);
    w.u32(unsigned(proofs.size()));
    for (const Proof &p : proofs) {
        if (w.oom) break;
        w.hex_var(p.id);
        w.i32(p.amount);
        w.hex_var(p.secret);
        w.hex_var(p.C);
        if (p.dleq) {
            w.u8(1);
            w.hex_fixed(p.dleq->e, 32);
            w.hex_fixed(p.dleq->s, 32);
            if (p.dleq->r) {
                w.u8(1);
                w.hex_fixed(*p.dleq->r, 32);
            } else {
                w.u8(0);
            }
        } else {
            w.u8(0);
        }
        if (p.witness) {
            w.u16(unsigned(p.witness->size()));
            w.raw(p.witness->data(), p.witness->size());
        } else {
            w.u16(0);
        }
    }
    if (w.oom) {
        if (w.owned) free(w.p);
        if (len) *len = 0;
        return nullptr;
    }
    if (len) *len = w.n;
    return w.p;
}

bool proofs_from_blob(const char *data, size_t len, std::vector<Proof> &out)
{
    out.clear();
    if (!blob_is_binary(data, len)) return false;
    BinReader r{data, 0, len};
    r.n = 4;
    unsigned count = r.u32v();
    // Bound by the blob's own size first: a corrupted count must not
    // drive a giant reserve (the OOM-abort boot-loop class). The minimal
    // NPB1 proof is ~20 bytes (empty strings, no dleq/witness).
    if (count > (len - 8) / 20) return false;
    out.reserve(count);
    for (unsigned i = 0; i < count && !r.bad; i++) {
        Proof p;
        p.id = r.hex_var();
        p.amount = r.i32v();
        p.secret = r.hex_var();
        p.C = r.hex_var();
        if (r.u8v()) {
            DLEQ d;
            d.e = r.hex_fixed(32);
            d.s = r.hex_fixed(32);
            if (r.u8v()) {
                d.r = r.hex_fixed(32);
            }
            p.dleq = std::move(d);
        }
        unsigned wlen = r.u16v();
        if (wlen) {
            std::string ws;
            ws.resize(wlen);
            if (!r.take(wlen, ws.data())) break;
            p.witness = std::move(ws);
        }
        if (r.bad || p.id.empty() || p.C.empty()) return false;
        out.push_back(std::move(p));
    }
    return !r.bad;
}

} // namespace cashu
