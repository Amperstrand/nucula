# Phone-vs-device HIL — design (cashu wallet NFC end-to-end on the ai-legion bench)

Status: implemented by `tests/hil/phone/` (suites T0–T4) + the sibling
suite in micronuts (`tests/hardware/phone/`, T5). CI wiring:
`.github/workflows/hil.yml` (workflow_dispatch + nightly opt-in).

## 1. Mission

Prove the cashu phone↔device token loop against REAL hardware, every
stage asserting observable evidence (console prints, adb/UI state,
mint-side truth):

- **phone → device**: the bench Samsung S20 FE (cashu.me v0.2.0, HCE
  service `com.cashu.me/.Core.NfcReceive.CashuNfcHostApduService`,
  NDEF AID `D2760000850101`) presents an NFC tag; a device-side reader
  reads/redeems it.
- **device → phone**: the device presents (card emulation / NDEF tag);
  the phone reads and receives/pays. Phone-initiated reads of a
  presented tag are the safe direction (see §5).
- **token plumbing**: tokens are minted host-side by the nucula rig
  payer (`rig/src/payer.rs`, `MINT_URL`, default bench mint
  `https://testnut.cashu.space` — fake sats, free rein per the money
  taxonomy) and handed to the phone over the bench browser-link page
  (adb `input text` and `am start -d` are lossy for ~400-char tokens —
  proven path from `micronuts docs/PHONE-BRINGUP-RUNBOOK.md`).

## 2. Topology as found (2026-10-21, live-verified)

Coordinator `ai-legion:20408` (runs ON the bench host, `ai-legion` =
192.168.13.208). Exporters of interest:

| Exporter | Resource | Place | Notes |
|---|---|---|---|
| ai-legion-nfc | `phone-s20/USBDevice` (`04e8:6860`, serial `RF8NB01ZYAH`) | `phone-s20` | adb over USB; cashu.me wallet live |
| ai-legion-nfc | `nucula-c3/SerialPort` (`usb-Espressif_...90:DA:72:9A:50:18-if00`, 115200) | `nucula-c3` | plain SerialPort — no ser2net on acquire |
| ai-legion-nfc | `m5stick-usb/SerialPort` (`usb-Hades2001_M5stack_49D6163EBE-if00-port0`, 115200) | `m5stick` | same |
| ai-legion-nfc | `ref-acr1252/USBDevice` (`072f:223b` @ fixed USB path) | `ref-acr1252` | reference reader (oracle, never DUT) |
| ai-legion-ccid | `nucula/NetworkService` (192.168.13.230:3232) | matched into `nucula-c3` + `ccid-nucula` | board OTA endpoint; acquiring either place excludes the sibling (coordinator-enforced) |
| ai-legion-ccid | `m5stick/NetworkService` (192.168.13.234:3232) | matched into `m5stick` + `ccid-m5stick` | same pattern |

Cross-project discipline: labgrid places are the ONLY enforcement
layer (owner decision 2026-10-20, PRTA #282); anything not acquired by
this session is not ours to touch.

## 3. Architecture

```
                 pytest (bench host or [self-hosted, bench] runner)
                 tests/hil/phone/
  fixtures: --hil gate (no accidental hardware in `cargo test`/PR CI)
  ┌────────────────────────────────────────────────────────────────┐
  │ place fixtures     labgrid-client acquire/release per suite     │
  │ nucula_console     pyserial, DTR/RTS-safe open, banner sync,    │
  │                    cmd()/expect() with full transcript         │
  │ phone              adb + uiautomator dump/tap (FLAG_SECURE:    │
  │                    no screenshots — text-bounds taps only)     │
  │ payer              rig `cargo test --features payer --ignored` │
  │                    (MINT_URL) → TOKEN=cashuB…                  │
  │ token_link         threaded http.server :8077, generated       │
  │                    <a href="cashu:…"> page; phone taps by      │
  │                    coordinates (web content is invisible to     │
  │                    uiautomator)                                 │
  └────────────────────────────────────────────────────────────────┘
        │                       │                      │
        v                       v                      v
  nucula-C3 console      Samsung S20 FE           testnut.cashu.space
  (PN7160 CE, "nfc       (cashu.me wallet,        (fake-sat mint; the
  request", console      HCE Send-by-NFC,         host-side oracle for
  prints "NFC payment    browser-link receive)    token truth)
  received: …")
```

Why plain `labgrid-client` subprocess instead of pytest-labgrid
fixtures: the plugin is not installed on the bench runner and the
client already wraps everything these suites need (acquire/release/show/
who). The ccid-firmware-rs HIL suites (the bench precedent,
`tests/hardware/labgrid/bench.py`) use the same shape.

### Console protocol facts (assertion targets)

- prompt: `nucula> ` (fresh boot prints it after the banner)
- `selftest` → `self-tests PASSED|FAILED`
- `mint add <url>` → `added mint [0]: <url>`; `status` lists mints
- `receive <token>` → `received <amt> in <n> proofs (<ms> ms)` (online)
- `balance` → `total: <amt>`; `invoice`/`melt` for the melt leg
- `nfc request <amount> [u=<unit>]` → async task, then
  `NFC payment received: <amt>` on success (nfc.cpp:224),
  `NFC token queued: … (offline; redemption pending)` when stashed,
  `payment timeout` after 120 s.

### Firmware matrix (which direction runs on which board)

| Board | NFC frontend | phone→device (reader) | device→phone (presents) |
|---|---|---|---|
| nucula-C3 (PN7160) | `main/nfc.cpp` (CE only) | — | `nfc request` (Type-4 tag, NUT-18 request) |
| nucula fork m5stick/atom (MFRC522) | `main/nfc_reader.cpp` (Type-2 NDEF reader) | reads T2 tags (NTAG/UL-class; phone HCE is Type-4 → UID-level only) | — |
| micronuts esp32-wallet (MFRC522, PR #108) | `src/nfc.rs` | ISO-14443-3 detect (`nfc: tag ATQA=… UID=…`) | — |

The full money loop phone→device today therefore ends at the ACR1252
reference reader (reads-only, §5) or a Type-2 presented tag; the full
device→phone loop is the nucula `nfc request` flow (T4).

## 4. Suites

| Suite | File | What it proves | Needs |
|---|---|---|---|
| T0 smoke | `test_t0_place_smoke.py` | places acquire/release, adb sees the phone, console banner (boot) | coordinator |
| T1 selftest | `test_t1_nucula_selftest.py` | `selftest` → PASSED over serial | nucula wallet fw on C3 |
| T2 lifecycle | `test_t2_wallet_lifecycle.py` | seed→mint add→rig-minted token→`receive`→`balance`→`invoice`+`melt`→`wipe`, all console-asserted | + bench WiFi, MINT_URL |
| T3 phone presents | `test_t3_phone_hce_reader.py` | phone HCE (Send-by-NFC) is a readable Type-4 tag carrying a spendable token; ACR1252 reads it (reads only) and the rig redeems at the mint | phone on ACR antenna (coupling probe, else skip) |
| T4 device presents | `test_t4_nucula_ce_phone.py` | `nfc request <amt>` on console; phone couples, receives the request, pays; console `NFC payment received:` | phone on PN7160 antenna (coupling probe, else skip) |
| T5 esp32-wallet | micronuts `tests/hardware/phone/` | esp32-wallet console up on m5stick; `nfc` verb sees the phone (ATQA/UID); wallet `receive` of a rig token | m5stick on wallet fw |

Skip policy: every hardware-gated stage self-skips with the precise
reason (place holder, board serving another image, no NFC coupling) —
recorded in the junit artifact. Never fake a stage.

## 5. Safety rules (bench doctrine, encoded in the harness)

1. **One console session per board per suite**: every fresh open of the
   C3's USB-CDC resets the board (rst:0x15 on host DTR patterns,
   ccid-firmware-rs #117). The harness opens with DTR/RTS deasserted
   BEFORE open (the `nucula-ccid-broker.py` safe-opener) and holds the
   port for the suite; one reset per suite start is the deterministic
   boot we assert against.
2. **The ccid broker swap dance** (when the C3 serves the ccid GemPC
   image instead of wallet fw): full `esptool read-flash` backup →
   `systemctl stop nucula-ccid-broker` → flash wallet fw → run suites →
   reflash backup → `systemctl start` → verify `Nucula CCID via broker`
   reappears in pcscd. Byte-exact restore; documented in the README.
3. **ACR1252U wedge rule** (instrumented 2026-09-07): rapid sequential
   CE-data WRITES wedge its CCID until physical replug. T3 performs
   SELECT + READ_BINARY sequences ONLY — reads are safe. No suite ever
   writes through the ACR. One CE scenario per power cycle.
4. **Exclusivity**: labgrid acquire/release around every hardware
   touch; anything not started by this session is not ours to kill
   (including the broker, pcscd reader configs, and other projects'
   board images).
5. **No secrets in commits**: bench WiFi credentials enter the nucula
   firmware build via gitignored `main/wifi_config.h`
   (`wifi_config.example.h` is the template); CI takes the mint URL
   from `vars`/defaults to the public fake-sat mint.

## 6. CI

`.github/workflows/hil.yml` — `workflow_dispatch` + nightly `schedule`
opt-in, `runs-on: [self-hosted, bench]`, NEVER `pull_request` (root
runner on a public-repo trigger would execute fork code on the bench
host — ccid #183 doctrine). Artifacts: junit xml, console transcripts,
uiautomator dumps, adb logcat excerpts. Job timeout + per-test
`--timeout-method=signal` (the thread method cannot kill a C-level
`select()`).

## 7. Known gaps / next steps

- No switched USB hub on the bench: phone→device reader-mode money
  flows that need a WRITE path (NDEF update to a presenting phone)
  stay on the PN7160 T4 CE flow only. A `uhubctl`-class switched port
  would let T3 also drive write-side scenarios safely isolated.
- T5 full redeem over MFRC522 is ISO-14443-3-level only today
  (`nfc.rs`); an ISO-DEP/T4CL-capable reader (PN532/PN7160 class) is
  the hardware upgrade for phone-HCE token reads on-device.
- Phone placement on the correct antenna is manual bench geometry;
  coupling probes make the suites skip loudly when the phone is
  elsewhere.
