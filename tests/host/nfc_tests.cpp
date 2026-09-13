// Host tests for the firmware's NFC NDEF engine (main/ndef.cpp) — the
// code that actually runs on the devices. Two roles:
// - emulated tag (XIAO): APDU engine serving SELECT/READ/UPDATE
// - receiver: UPDATE BINARY chunk reassembly into a token callback

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ndef.hpp"

void ndef_test_reset(void);

static int g_failed = 0;
static void expect(const char *name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_failed++;
}

// APDU driver for the emulated tag.
struct Rsp {
    uint8_t buf[NDEF_MAX_DATA_SIZE + 8];
    size_t len = 0;
    uint16_t sw() const
    {
        return len >= 2 ? ((uint16_t)buf[len - 2] << 8) | buf[len - 1] : 0;
    }
    const uint8_t *data() const { return buf; }
    size_t data_len() const { return len >= 2 ? len - 2 : 0; }
};

static Rsp apdu(const uint8_t *a, size_t n)
{
    Rsp r;
    ndef_handle_apdu(a, n, r.buf, &r.len);
    return r;
}

static const uint8_t SEL_AID[] = {
    0x00, 0xA4, 0x04, 0x00, 0x07,
    0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00,
};
static const uint8_t SEL_HDR[] = {0x00, 0xA4, 0x00, 0x0C};
static const uint8_t READ_HDR[] = {0x00, 0xB0};
static const uint8_t UPD_HDR[] = {0x00, 0xD6};

static Rsp sel_file(uint16_t fid)
{
    uint8_t a[7] = {SEL_HDR[0], SEL_HDR[1], SEL_HDR[2], SEL_HDR[3], 0x02,
                    (uint8_t)(fid >> 8), (uint8_t)fid};
    return apdu(a, sizeof(a));
}

static Rsp read_bin(uint16_t off, uint8_t le)
{
    uint8_t a[5] = {READ_HDR[0], READ_HDR[1], (uint8_t)(off >> 8),
                    (uint8_t)off, le};
    return apdu(a, sizeof(a));
}

// Received-message capture for the UPDATE flow.
static std::vector<std::vector<uint8_t>> g_received;
static void on_message(const uint8_t *data, size_t len)
{
    g_received.emplace_back(data, data + len);
}

int main()
{
    // ------------------------------------------------------------------
    // Emulated tag: serve a message through the full Type 4 read flow.
    // ------------------------------------------------------------------
    ndef_test_reset();
    ndef_set_message("hello over tag");

    expect("SELECT AID -> 9000", apdu(SEL_AID, sizeof(SEL_AID)).sw() == 0x9000);
    expect("SELECT CC -> 9000", sel_file(0xE103).sw() == 0x9000);
    Rsp cc = read_bin(0, 15);
    expect("READ CC -> 15B + 9000", cc.sw() == 0x9000 && cc.data_len() == 15);
    // v2.0 mapping CC: CCLEN(2) version(1) MLe(2) MLc(2) then the NDEF
    // File Control TLV carrying E104.
    expect("CC: CCLEN 15, version 2.0, E104 control TLV",
           cc.data_len() >= 10 &&
           cc.data()[0] == 0x00 && cc.data()[1] == 0x0F &&
           cc.data()[2] == 0x20 &&
           cc.data()[7] == 0x04 && cc.data()[8] == 0x06 &&
           cc.data()[9] == 0xE1 && cc.data()[10] == 0x04);

    expect("SELECT NDEF (loaded) -> 9000", sel_file(0xE104).sw() == 0x9000);
    Rsp nlen = read_bin(0, 2);
    uint16_t n = nlen.data_len() == 2 ? ((uint16_t)nlen.data()[0] << 8) | nlen.data()[1] : 0;
    expect("READ NLEN sane", nlen.sw() == 0x9000 && n > 0 && n < NDEF_MAX_DATA_SIZE);

    Rsp file_head = read_bin(0, (uint8_t)(n + 2));
    std::string text;
    expect("served file parses back (NLEN form)",
           file_head.sw() == 0x9000 && file_head.data_len() == n + 2 &&
           ndef_parse_message(file_head.data(), file_head.data_len(), text) &&
           text == "hello over tag");

    // Out-of-bounds read rejected.
    expect("READ beyond file -> 6A82", read_bin(0, 250).sw() == 0x6A82 ||
          // (CC is 15 bytes; 250 must fail — but with NDEF selected 250
          // may fit; force CC selection where 250 > 15.)
          (sel_file(0xE103), read_bin(0, 250).sw() == 0x6A82));

    // ------------------------------------------------------------------
    // Protocol errors.
    // ------------------------------------------------------------------
    ndef_test_reset(); // no message loaded
    apdu(SEL_AID, sizeof(SEL_AID));
    expect("SELECT NDEF unloaded -> 6A82", sel_file(0xE104).sw() == 0x6A82);
    expect("SELECT unknown file -> 6A82", sel_file(0xE105).sw() == 0x6A82);
    uint8_t upd_cc[] = {0x00, 0xD6, 0x00, 0x00, 1, 0xAA};
    sel_file(0xE103);
    expect("UPDATE CC forbidden -> 6A82", apdu(upd_cc, sizeof(upd_cc)).sw() == 0x6A82);
    uint8_t upd_nofile[] = {0x00, 0xD6, 0x00, 0x00, 1, 0xAA};
    sel_file(0xE105); // rejected select leaves SEL_NONE
    expect("UPDATE no file -> 6A82", apdu(upd_nofile, sizeof(upd_nofile)).sw() == 0x6A82);
    uint8_t garbage[] = {0x80, 0xE0, 0x00, 0x00};
    expect("unknown APDU -> 6A82", apdu(garbage, sizeof(garbage)).sw() == 0x6A82);

    // ------------------------------------------------------------------
    // Receiver: UPDATE BINARY reassembly fires the callback exactly once.
    // ------------------------------------------------------------------
    ndef_test_reset();
    g_received.clear();
    ndef_set_receive_callback(on_message);
    apdu(SEL_AID, sizeof(SEL_AID));
    sel_file(0xE104); // file select ok even unloaded? no: 6A82, and
                      // SEL stays NONE — the phone flow requires a
                      // loaded message; exercise the real path instead:
    ndef_set_message("placeholder");
    sel_file(0xE104);

    // Build a small NDEF text file and write it NLEN-first in chunks.
    const std::string token = "cashuBrelayPayload123";
    size_t payload = 3 + token.size(); // status + "en" + text
    std::vector<uint8_t> rec = {0xD1, 0x01, (uint8_t)payload, 0x54, 0x02, 'e', 'n'};
    rec.insert(rec.end(), token.begin(), token.end());
    std::vector<uint8_t> file = {(uint8_t)(rec.size() >> 8), (uint8_t)rec.size()};
    file.insert(file.end(), rec.begin(), rec.end());

    // Chunk 1: NLEN + first body bytes (single write completes it).
    uint8_t w1[NDEF_MAX_DATA_SIZE + 8] = {UPD_HDR[0], UPD_HDR[1], 0x00, 0x00,
                                          (uint8_t)file.size()};
    memcpy(&w1[5], file.data(), file.size());
    Rsp r1 = apdu(w1, 5 + file.size());
    expect("single-write UPDATE -> 9000 + callback", r1.sw() == 0x9000 && g_received.size() == 1);
    if (!g_received.empty()) {
        auto &m = g_received[0];
        expect("callback bytes = NLEN + record",
               m.size() == file.size() && memcmp(m.data(), file.data(), m.size()) == 0);
        std::string text2;
        expect("re-assembled message parses to the token",
               ndef_parse_message(m.data(), m.size(), text2) && text2 == token);
        expect("token extracted from re-assembly",
               ndef_extract_cashu_token(text2) == "cashuBrelayPayload123");
    }

    // Multi-chunk, phone pattern: NLEN=0 init, body chunks from
    // offset 2, then the final NLEN write completes.
    ndef_test_reset();
    g_received.clear();
    ndef_set_receive_callback(on_message);
    ndef_set_message("placeholder2");
    apdu(SEL_AID, sizeof(SEL_AID));
    sel_file(0xE104);
    uint8_t init[] = {UPD_HDR[0], UPD_HDR[1], 0x00, 0x00, 2, 0x00, 0x00};
    apdu(init, sizeof(init)); // NLEN = 0: body follows
    expect("NLEN=0 init does not complete", g_received.empty());
    size_t body = file.size() - 2;
    uint8_t c1[NDEF_MAX_DATA_SIZE] = {UPD_HDR[0], UPD_HDR[1], 0x00, 0x02,
                                      (uint8_t)body};
    memcpy(&c1[5], file.data() + 2, body);
    apdu(c1, 5 + body); // whole body; NLEN still 0 -> not complete
    expect("body write with NLEN=0 does not complete", g_received.empty());
    uint8_t fin[] = {UPD_HDR[0], UPD_HDR[1], 0x00, 0x00, 2,
                     file[0], file[1]};
    apdu(fin, sizeof(fin)); // final NLEN -> fires
    expect("final NLEN write completes -> callback", g_received.size() == 1);

    // Truncated UPDATE rejected without state damage.
    ndef_test_reset();
    g_received.clear();
    ndef_set_receive_callback(on_message);
    ndef_set_message("placeholder3");
    apdu(SEL_AID, sizeof(SEL_AID));
    sel_file(0xE104);
    uint8_t trunc[] = {0x00, 0xD6, 0x00, 0x00, 8, 0x01}; // lc=8, only 1 byte
    expect("truncated UPDATE -> 6A82", apdu(trunc, sizeof(trunc)).sw() == 0x6A82);

    // ------------------------------------------------------------------
    // Token extraction strategies (the device twin of the rig tests).
    // ------------------------------------------------------------------
    expect("extract: bare cashuA",
           ndef_extract_cashu_token("cashuAUt4st") == "cashuAUt4st");
    expect("extract: #token= cashuB",
           ndef_extract_cashu_token("https://x.example/#token=cashuBAbC") == "cashuBAbC");
    expect("extract: ?token= cashuA",
           ndef_extract_cashu_token("https://x.example/?token=cashuAAq1") == "cashuAAq1");
    expect("extract: embedded cashuB",
           ndef_extract_cashu_token("junk cashuBZz9 trailing") == "cashuBZz9");
    expect("extract: none", ndef_extract_cashu_token("no tokens here").empty());

    // Long-record parse: >255 payload (SR cleared, 4-byte length),
    // NLEN-prefixed file form.
    std::string big(300, 'y');
    {
        std::vector<uint8_t> p2 = {0x02, 'e', 'n'};
        p2.insert(p2.end(), big.begin(), big.end());
        std::vector<uint8_t> long_rec = {0xC1, 0x01}; // MB|ME, SR=0, TNF=1
        uint32_t pl = (uint32_t)p2.size();
        long_rec.push_back((pl >> 24) & 0xFF);
        long_rec.push_back((pl >> 16) & 0xFF);
        long_rec.push_back((pl >> 8) & 0xFF);
        long_rec.push_back(pl & 0xFF);
        long_rec.push_back(0x54);
        long_rec.insert(long_rec.end(), p2.begin(), p2.end());
        std::vector<uint8_t> file3 = {(uint8_t)(long_rec.size() >> 8),
                                      (uint8_t)long_rec.size()};
        file3.insert(file3.end(), long_rec.begin(), long_rec.end());
        std::string text3;
        expect("long-record parse (300-char payload)",
               ndef_parse_message(file3.data(), file3.size(), text3) && text3 == big);
    }

    printf("\n%s (%d nfc test(s) failed)\n", g_failed ? "FAILED" : "ALL PASS", g_failed);
    return g_failed ? 1 : 0;
}
