# Rig inventory & tonight's gap map — 2026-10-15

Read-only audit for the test-framework session. Sources: `AGENTS.md`,
`rig/README.md`, `rig/src/*.rs`, `rig/tests/e2e.rs`, `rig/examples/*.rs`,
`rig/labgrid/*`, `rig/scripts/*.py`, `main/commands_*.cpp`, `main/console.cpp`.
Unit-test run recorded in §D. No hardware touched, nothing flashed, no commits.

---

## A) EXISTING CAPABILITIES

### A.1 Firmware console command tree (every command + args)

Registrations: `main/console.cpp:216` (`help`), `main/commands_wallet.cpp:599-607`,
`main/commands_seed.cpp:106`, `main/commands_system.cpp:511-529`.
Shared option parsing `u=` `m=` `a=` `w=` + bare-int mint index: `main/commands_common.cpp:10-47`.

**Wallet** (`main/commands_wallet.cpp`):

| Command | Args | Handler | Registration |
|---|---|---|---|
| `status` | — | `cmd_status` :29 (wifi/nfc/heap/unit/mints/keysets/proofs/totals) | :599 |
| `balance` | — | `cmd_balance` :71 (per-proof, subtotals, totals) | :600 |
| `unit` | `[<name>]` show/set persisted default unit | `cmd_unit` :107 | :601 |
| `receive` | `<cashu token>` (cashuA/cashuB; swap + balance) | `cmd_receive` :151 | :602 |
| `mint` | `[list\|add <url>\|remove <index\|url>\|info [idx]]` | `cmd_mint` :200 (add :220, info :249 incl. method/unit/min-max rows, remove :294) | :603 |
| `invoice` | `<amount> [u=<unit>] [m=<method>] [w=<mint_idx>]` | `cmd_invoice` :325 | :604 |
| `claim` | `<quote_id> [m=<method>] [w=<mint_idx>]` | `cmd_claim` :382 | :605 |
| `melt` | `<request> [u=<unit>] [m=<method>] [a=<amount>] [w=<mint_idx>]` | `cmd_melt` :458 | :606 |
| `stickup` | — drain all funds as V4 tokens | `cmd_stickup` :528 | :607 |

**Seed** (`main/commands_seed.cpp`): `seed [show|generate|restore <12 words>|wipe]`
— `cmd_seed` :21, usage :101, registered :106 (NUT-13; generate/restore/wipe erase all wallets :15-19).

**System** (`main/commands_system.cpp`):

| Command | Args | Handler | Registration |
|---|---|---|---|
| `nfc` | `[request <amount> [u=<unit>]\|stop]`; bare = status | `cmd_nfc` :40 (request :46-67, stop :68-73) | :511 |
| `keypad` | `scan` (30 s PCF8574 probe) | `cmd_keypad` :168 | :512 |
| `i2cscan` | `[sda scl]` (main bus + pin-pair sweep on classic ESP32) | `cmd_i2cscan` :289 | :513 |
| `i2crecover` | `<clocks>` (default 32, ≤4096; SCL-clock stuck bus) | `cmd_i2crecover` :446 | :514 |
| `i2cdump` | `<sda> <scl> <addr-hex>` regs 00-3F — **atom/m5stick only** (`#if` :218/:515) | `cmd_i2cdump` :241 | :516 |
| `nfcdump` | `[pages] [start]`, pages+start ≤64 — **atom/m5stick only**; T4 tags dump NDEF via ISO-DEP (:355-412), T2 tags dump Ultralight pages (:414-426) | `cmd_nfcdump` :331 | :517 |
| `display` | `<on\|off\|fill [rgb565-hex]>` — **m5stick only** (`#if` :463) | `cmd_display` :464 | :520 |
| `button` | — 5 s press window — **m5stick only** | `cmd_button` :488 | :521 |
| `wstat` | — wallet-persistence witness: saves/fails/last_err/blob/proofs_loaded + NVS entry stats | `cmd_wstat` :431 | :523 |
| `reboot` | — | `cmd_reboot` :77 | :524 |
| `heap` | — free / largest block / min-ever | `cmd_heap` :89 | :525 |
| `tasks` | — per-task prio + stack high-water | `cmd_tasks` :99 | :526 |
| `log` | `<e\|w\|i\|d> [tag]` | `cmd_log` :117 | :527 |
| `bench` | — crypto primitive benchmark | `cmd_bench` :138 | :528 |
| `selftest` | — crypto/keyset/unit/json/wallet self-tests | `cmd_selftest` :146 | :529 |

Plus `help` — `main/console.cpp:216`.

### A.2 Rig modules and what they drive

Crate `nucula-rig` (`rig/Cargo.toml`; features `live` = pcsc+serialport, `payer` = cdk stack). Module gates `rig/src/lib.rs:11-21`.

- **`ndef_t2t.rs`** (always compiled): T2T NDEF image build/parse.
  `build_ndef_text_record` :36 (short/long form), `build_ndef_text_image` :59
  (CC `E1 40 area/8 00`, 3-byte TLV ≥254, `0xFE` terminator, area 8..2040),
  `parse_ndef_text` :89, `extract_cashu_token` :143 (bare / `#token=` /
  `token=` / embedded — mirrors device extraction), `NDEF_AREA_512` :11.
- **`acr.rs`** (`live`): ACR1252U over PC/SC escapes.
  Escape ioctl `0x42000001` :16, `E1` framing :80-89. Reader-level:
  `open` :69 (first `PICC`, DIRECT share), `firmware` :91,
  `set/read_picc_operating_parameter` :99/:104 (NVM),
  `set/read_auto_polling` :110/:115 (NVM, factory `0x8F`),
  `exit_card_emulation` :126 (**NEVER CALL** — documented wedge).
  Card emulation: `write_ce_data` :134 / `read_ce_data` :152 (one-byte
  StartOffset), `enter_ultralight_emulation` :173 (one-way per power
  cycle), `present_ndef_image` :200 — trims at `0xFE`, **≤251 B one-write
  budget** (`ImageTooLarge` :30), quiets polling + 1 s settle, single
  write, 48 B readback verify, then enter CE. Reader-mode Type 4:
  `Acr1252Card::connect` :279 (Shared, T=1), `apdu` :294 (GetResponse
  chains), `write_ndef` :323 (select NDEF app `D276…`, CC parse +
  capacity check via `cc_ndef_capacity` :397 v1/v2.x, FID `E104` with CC
  fallback, NLEN + MLc-chunked `D6` updates, NLEN readback), errors
  `CardError` :232-270 incl. `Capacity{need,have}` :242.
- **`atom_console.rs`** (`live`): serial console driver, 115200, prompt
  `nucula> ` :10. `open` :45 (DTR/RTS true + resync), `cmd` :76 (single
  prompt-sync), `port_mut` :82 (long captures), `read_until` :94,
  helpers `status`/`mint_add`/`nfc_request`/`nfc_stop`/`wait_for_log`
  :123-147.
- **`locks.rs`** (always): machine-wide flocks `/tmp/amperstrand-rig/<device>.lock`
  (:17), non-blocking exclusive + pid :56-90, released on drop :93.
- **`rig.rs`** (`live`): orchestration. Constants :22-27 — coordinator
  `192.168.13.221:20408`, place `nucula-rig`, client
  `/home/ubuntu/.local/bin/labgrid-client`, mint
  `http://192.168.13.221:3338`. `labgrid()` acquire/release :48-54.
  `preflight(repair)` :68 — ACR fw + polling restore `0x00→0x8F` :74-85,
  atom console + stale `nfc stop` + wifi :88-111, mint TCP reachability
  :113-122; never touches mode-switch escapes. `RigGuard::acquire` :138
  (labgrid acquire → flock `acr1252` + flock `atom-9d529068b4` →
  preflight(true)); `Drop` :160 (polling `0x8F`, `nfc stop`, place
  release) — even on panics.
- **`payer.rs`** (`payer`): `mint_token` :46 — cdk wallet (memory store,
  random seed), BOLT11 quote, `wait_and_mint_quote` ≤20 s (FakeWallet
  auto-settle), `prepare_send`/`confirm` → token string. Ignored tests
  :77-108 (`MINT_URL`, default `127.0.0.1:3338`).
- **`strip.rs`** (`payer`): `strip_dleq` :286 — hand-rolled definite-length
  CBOR walker (MAX_DEPTH 32 :28), drops per-proof `"d"` maps only,
  byte-identical elsewhere; ~374→~230 chars for 1 sat.
- **Board plumbing** (`lib.rs`): `Board::{Atom,M5Stick}` :27-49 with
  by-id default ports; `detect_board` :53 (`NUCULA_BOARD` env or port
  probe); `atom_default_port` :70 (`ATOM_PORT` override).

### A.3 Existing e2e tests + exact invocation

All in `rig/tests/e2e.rs`, `#![cfg(feature = "live")]` :19, all `#[ignore]`d
(hardware-gated). Header invocation: `rig/tests/e2e.rs:13`,
`rig/README.md:125`:

```bash
cargo test --features live,payer --test e2e -- --ignored --nocapture
```

| Test | What it proves | Line |
|---|---|---|
| `atom_console_smoke` | serial driver round-trip `status` | :38 |
| `relay_token_over_the_air_via_boltcard` | **receive-direction relay, real card**: mint 1 sat → ACR reader-mode `write_ndef` onto boltcard → polling quieted → atom T4T read → `redeemed` | :55 |
| `atom_rf_iso14443_smoke` | REQA answered + one completed anticollision round in a reader session | :108 |
| `console_relay_money_loop` | full money loop minus RF: mint → `receive` over serial → swap (NUT-12) → `balance` +1 sat | :137 |
| `relay_acr_emulated` | full-token ACR-CE relay — **blocked by design**: asserts image ≤52 B (served geometry) and fail-fasts otherwise; DLEQ strip opt-in via `NUCULA_CE_STRIP_DLEQ=1` :180-211 | :174 |
| `relay_acr_emulated_small_payload` | sub-ceiling CE→RF→T2T-read→NDEF-extract chain; asserts the `no cashu token in: <payload>` log | :256 |
| `relay_stashes_offline_then_drains_on_reconnect` | offline stash → manual AP restore → drain (manual step mid-test) | :310 |
| `console_line_probe` | CP210x (dtr,rts) diagnostics | :348 |

Helper `sat_total` :294. Env: `ATOM_PORT`, `MINT_URL` (:15-17).
Preconditions (README:128-139): mint up, writable blank boltcard
sandwiched, ACR freshly powered.

### A.4 Labgrid / locking wiring

- **Place**: `rig/labgrid/nucula-place.sh` — idempotent create against
  coordinator `192.168.13.221:20408` (:10), matches three EXISTING
  exporter resources (:22-24): `ai-legion-small-microfips/atom-b-serial`,
  `ai-legion-small-microfips/m5stick-nucula-serial`,
  `ai-legion-small/acr1252` — acquiring excludes both other projects.
- **Pytest env**: `rig/labgrid/labgrid-env.yaml` (`RemotePlace nucula-rig`,
  `--lg-coordinator 192.168.13.221:20408` :8) importing client-side
  `BenchSerialToken` (`rig/labgrid/labgrid_resources.py:10-16`) —
  acquisition/exclusivity records only; direct pcscd/serial access stays
  the working path.
- **Rig-side use**: `rig.rs:48-54` shells `labgrid-client -x … -p nucula-rig
  acquire|release`; flocks always taken alongside ("labgrid place + flock"
  vs "flock only", `rig.rs:145-150`). Teardown releases :169-171.
- **Stale holds**: `labgrid-client who` reveals them (README:170-173).

### A.5 Soak / boundary examples and what they measure

- **`rig/examples/soak.rs`** — overnight console money loops. Invocation
  :4: `cargo run --features live,payer --example soak -- [--hours 8]`
  (env `NUCULA_SOAK_HOURS` :164-168, `MINT_URL` :170). Amounts 1/2/5/21/42
  (:25), every 50th cycle 100-sat probe tracking the console-transport
  ceiling (:20-27 — 100+ sat tokens break the console stream
  deterministically). `heap`/`tasks` snapshot every 10th (:28, :259-269);
  reboot + NVS balance-persistence verify every 25th (:29, :271-297);
  5 consecutive failures abort (:30, :233-252); recovery via
  `scripts/recover_console.py` pyserial boot-line pulse (:94-107 — the
  in-Rust RTS toggle does not reach the Hades2001 bridge); `cmd_retry`
  absorbs bridge chunk drops (:126-138); JSONL events to
  `rig/results/soak-<ts>.jsonl` (:58-72, :172-189); crash-consistency
  forensics `log i wallet_nvs` at start (:197-199); RigGuard held for the
  whole run (:191). Measures: wallet correctness over cycles, balance
  deltas, heap/task drift, NVS persistence across reboots, bridge flake
  rate, wallet-reset boundaries.
- **`rig/examples/boundary_probe.rs`** — drives balance toward the
  ~790-sat persistence-failure boundary: 10 × receive 42 sat, `wstat`
  checkpoints (`saves=`, `nvs entries`) every 3rd (:17-39). Measures:
  NVS save failures / blob growth vs balance.
- **`rig/examples/console_dump.rs`** — 90 s passthrough capture for slow
  dumps (`nfcdump 68`, :4).
- **`rig/scripts/analyze_soak.py`** — reconstructs the campaign across
  slices: per-slice cycle/fail tallies, balance series, every balance
  DECREASE (= wallet-state-reset boundaries) with context (:19-81).

### A.6 Known hardware blockers (as documented)

1. **ACR1252U CE-write wedge** (AGENTS.md "The ACR1252U wedge";
   `acr.rs:187-199`; reproducer `rig/scripts/acr_instrumented.py`):
   rapid sequential CE-data writes — 48 B writes 1-3 apply (~142 ms),
   write 4 short-echo partial (36/48, SW 9000), write 5 kills CCID until
   **physical replug**. One write per power cycle never wedged →
   `present_ndef_image` = ONE trimmed write ≤251 B; one CE scenario per
   power cycle; wedged signature `Pcsc(NotTransacted)` then
   "Reader is unavailable"; nothing software revives it.
2. **CE one-way door** (README:90-105): after `enter`, all CCID dead
   until replug (port reset, set_configuration, SAM, interrupt drain,
   sysfs authorized all fail).
3. **Ultralight CE served geometry** (README:64-88; `e2e.rs:201-211`):
   CE serves only the 16-page Ultralight geometry over RF — data pages
   3..15 = **52 bytes** — so full tokens (≥239 B stripped image) cannot
   relay via ACR CE on this reader.
4. **Carrier capacity** (README:43-51): relay boltcard NDEF file 256 B
   silicon-enforced; DLEQ-bearing 1-sat token = 386 B NDEF → no valid
   token fits; `write_ndef` fails fast `Capacity{need,have}`.
5. **Sandwich RF coupling** (README:53-62): stacked antennas — REQA
   ~100 %, anticollision ~25/120 s, 9-byte SELECT 0/75 (silent timeouts);
   separate antennas a few cm to restore.
6. **M5Stick flash/console gotchas** (AGENTS.md): flash at 115200 with
   explicit `--port /dev/serial/by-id/usb-Hades2001_M5stack_49D6163EBE-if00-port0`
   (ttyUSB0 is the CYD!), `sudo systemctl stop bolty-console` for the
   flash window; build env `IDF_TARGET=esp32 SDKCONFIG=build-m5stick/sdkconfig
   SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.defaults.m5stick'`;
   silent console → pyserial `dtr=False`, pulse `rts` True→False;
   RC522 latch-up cleared by AXP192 EXTEN rail power-cycle in firmware.
7. **Hades2001 bridge chunk drops** (`soak.rs:91-107`;
   `scripts/recover_console.py:1-11`): dense console traffic drops TX/RX
   chunks; recovery only via the pyserial boot-line path.
8. **Console transport ceiling** (`soak.rs:20-24`): 100+ sat tokens
   (~1.5 KB lines) complete on-device but break the console output
   stream deterministically.
9. **Wallet persistence boundary ~790 sats** (`boundary_probe.rs:1-4`).

---

## B) TONIGHT GAP MAP

### (i) Repeatable receive-direction relay: ACR writes real NTAG, reader reads it

**EXISTS**
- Whole e2e for a **Type 4** carrier (NTAG 424 DNA class boltcard):
  `relay_token_over_the_air_via_boltcard` (`e2e.rs:55-97`) — mint,
  ACR reader-mode `write_ndef` (`acr.rs:323-388`), polling
  quiet/restore choreography, atom T4T/ISO-DEP read + redeem.
- Device-side T4T reader proven (`commands_system.cpp:355-412`
  `nfcdump` ISO-DEP branch; README ISO-DEP activation evidence).
- Payer minting (`payer.rs:46`), guard/preflight/teardown (`rig.rs:138-173`).

**PARTIAL**
- *Repeatability*: the test is single-shot — no N-iteration loop, no
  per-run JSONL artifact (only `eprintln`), no automatic
  placement/readback sanity step, card placement + fresh ACR power cycle
  are manual preconditions (README:128-139).
- If "real NTAG" means NTAG424 (T4T): only the loop harness is missing.

**MISSING**
- **Type 2 NTAG (NTAG21x) write path on the ACR** — `acr.rs` has no T2T
  field writer (`write_ce_data` :134 is the CE preload, not a field
  write). T2T writes are native MIFARE commands, not the APDU `write_ndef`
  path — new code + a real NTAG sticker on the bench.
- Soak-style RF relay loop with per-iteration logging (reuse `soak.rs`
  event/JSONL machinery).

### (ii) Repeatable send-direction: XIAO-CE presents token + ACR reader-mode reads

**EXISTS (reusable pieces only)**
- ACR reader-mode T=CL connect + APDU engine (`acr.rs:279-317`) — enough
  to build an NDEF *read* helper on top of the CC/NLEN reads already in
  `write_ndef` (:331, :379-385).
- PN7160 card emulation is the XIAO firmware's own feature (root README
  hardware table); T2T image builder for reference payloads
  (`ndef_t2t.rs:59`).

**PARTIAL**
- Nothing: the rig has no XIAO touchpoint at all.

**MISSING**
- `Board::Xiao` + default port / `NUCULA_BOARD` value (`lib.rs:27-67`);
  labgrid match for a XIAO serial (`nucula-place.sh:22-24` covers
  atom/m5stick/acr only); no XIAO registered on this bench
  (README:193-201 hardware map lists Atom + ACR only).
- Firmware console **CE-present/pay command** — `nfc` is receive/reader
  mode only (`commands_system.cpp:40-75`); NUT-18 send-direction surface
  is not console-exposed.
- Host-side `read_ndef` on `Acr1252Card` (mirror of `write_ndef`).
- The e2e itself (present via XIAO CE → ACR reads → verify token text).

### (iii) Console money-loop battery on both M5 boards

**EXISTS**
- `console_relay_money_loop` (`e2e.rs:137-162`): mint → `receive` →
  swap → balance assertion, on whichever `ATOM_PORT`/`NUCULA_BOARD`
  resolves.
- Both boards plumbed: `Board` + by-id ports + `ATOM_PORT`/`NUCULA_BOARD`
  (`lib.rs:27-75`); m5stick labgrid match (`nucula-place.sh:23`).
- M5Stick console recovery (`scripts/recover_console.py`), retry
  machinery from soak (`soak.rs:109-150`).

**PARTIAL**
- Battery scope: one amount (1 sat), one flow (`receive`) per run — no
  amount matrix, no `invoice`/`claim`/`melt`/`stickup`/`seed` coverage in
  any rig test.
- Per-board tagging: results not labeled by board; flock name is
  hard-coded `atom-9d529068b4` regardless of board (`rig.rs:144`) —
  exclusivity still works (single global name) but conflicts are
  mislabeled on M5Stick runs.
- Labgrid place matches BOTH serials at once, so any acquire locks both
  boards (fine for exclusivity; prevents splitting boards across
  parallel projects).

**MISSING**
- A battery runner: per-board × per-amount × per-command matrix with
  balance-delta assertions and JSONL artifacts (extend soak or a new
  example). Melt needs a payable target — the payer currently only
  mints (`payer.rs:46-67`); a FakeWallet-side invoice helper or
  `invoice`+`claim` pairing must be worked out.

### (iv) Overnight soak with witness/perf sampling

**EXISTS**
- Complete console-transport soak: `rig/examples/soak.rs` (hours arg,
  JSONL, heap/tasks every 10, reboot-persistence every 25, recovery +
  abort thresholds, bridge-flake absorption) + `analyze_soak.py` +
  `boundary_probe.rs`. See §A.5.

**PARTIAL**
- Witness sampling is heap/tasks/balance only; `wstat`
  (`commands_system.cpp:431-444` — saves/fails/last_err/blob/NVS
  entries) is NOT in the soak snapshot cadence (only boundary_probe
  uses it); `log i wallet_nvs` forensics lines are captured to the
  console but not parsed into events.
- Soak covers the console path only — no RF-direction soak exists.

**MISSING**
- Small edit: add `wstat` (and optionally parsed wallet_nvs lines) to
  the `SNAPSHOT_EVERY` cadence in `soak.rs:259-269`.
- RF-relay soak variant — blocked on (i) repeatability work.

### (v) Parity-matrix inputs for micronuts (feature/command surface list)

**EXISTS**
- Complete console command surface with args — §A.1 of this document.
- NUT coverage list (NUT-00/02/03/04/05/10/11/12/13/18/23) in the root
  README "Protocol" section.
- Live mint surface probe: `mint info` prints method/unit/min-max rows
  straight from the mint (`commands_wallet.cpp:249-292`).
- cdk-side exercise of mint endpoints (quote/mint/swap) via
  `payer.rs:46-67`.

**PARTIAL**
- The surface is documented but scattered (README tables + code + this
  inventory); no single generated artifact; the mint itself is
  out-of-repo (`micronuts-mint`) and its startup script
  `/tmp/opencode/rig/start-mint.sh` (README:130-133, `rig.rs:121`) is
  **not currently present on this host** — /tmp is ephemeral; it must be
  recreated before any live run tonight.

**MISSING**
- The parity-matrix artifact itself: command/feature ×
  (firmware console | cdk payer | micronuts endpoint | rig test
  coverage). Build tonight from §A.1 + `mint info` output + payer code
  paths; a small generator script (or a table in this docs tree) is the
  deliverable.

---

## C) EXACT COMMANDS for tonight (fresh operator)

### C.0 One-time environment (verified this session)

```bash
# 1. Rust: system cargo (1.75) cannot parse rig/Cargo.lock v4 — use rustup:
export PATH="$HOME/.cargo/bin:$PATH"        # stable 1.99.0 verified

# 2. cdk path dependency: rig/Cargo.toml:20-21 expects /home/ubuntu/src/cdk.
#    Bridged this session: /home/ubuntu/src/cdk -> /root/src/cdk-0.18.1
#    (checkout is 0.18.1; Cargo.lock pins 0.18.0 — the first cargo resolve
#    drifts rig/Cargo.lock 0.18.0->0.18.1. Accept the drift tonight or
#    `git restore rig/Cargo.lock` after runs; it was restored post-audit.)

# 3. pcscd must run and libccid ifdDriverOptions must have bit 0 set
#    (0x0003 in ifd-ccid Info.plist + pcscd restart) — see rig/README.md:18-26.

# 4. Mint: /tmp/opencode/rig/start-mint.sh is MISSING on this host right now.
#    Recreate it (adapter 0.0.0.0:3338, FakeWallet, per rig/README.md:130-133)
#    before any payer/live test. Then verify:
curl -s http://192.168.13.221:3338/ >/dev/null && echo mint-up

# 5. labgrid place (idempotent; coordinator restarts wipe places):
bash rig/labgrid/nucula-place.sh
labgrid-client -x 192.168.13.221:20408 who     # check for stale holds
```

Lock acquisition is automatic: every live e2e / soak / boundary run wraps
itself in `RigGuard::acquire()` (labgrid place + `/tmp/amperstrand-rig`
flocks + preflight; `rig.rs:138`). Manual only for ad-hoc console work:

```bash
labgrid-client -x 192.168.13.221:20408 -p nucula-rig acquire
# ... work ...
labgrid-client -x 192.168.13.221:20408 -p nucula-rig release
```

### C.1 Unit tests (no hardware) — recorded result

```bash
cd rig && cargo test
```
Result 2026-10-15 (rustup stable 1.99.0): **9 passed; 0 failed; 0 ignored**
— `ndef_t2t::tests` (7: extraction_covers_all_device_strategies,
image_layout_matches_device_expectations, oversize_record_is_rejected,
rejects_non_ndef_images, round_trips_through_the_parser,
three_byte_tlv_length_form, token_survives_build_parse_extract) +
`locks::tests` (2: exclusive_and_released_on_drop,
independent_devices_do_not_conflict). Integration target `tests/e2e.rs`
compiles empty (0 tests) without `--features live`; doc-tests 0.

To also run the CBOR stripper units (still no hardware; `strip` is gated
behind `payer`, `lib.rs:80-81` — 7 more tests, plus 2 `payer` tests that
stay `#[ignore]`d without a mint):

```bash
cargo test --features payer lib
```

### C.2 Payer smoke against the mint (needs mint up; no NFC)

```bash
cd rig
MINT_URL=http://192.168.13.221:3338 \
  cargo test --features payer -- --ignored --nocapture
# runs: payer::tests::mints_a_token_from_the_running_mint,
#       payer::tests::one_sat_token_fits_the_emulated_tag_area
```

### C.3 Live e2e (per test; ATOM board is the default port)

```bash
cd rig
# Board select: M5Stick instead of the atom:
#   export NUCULA_BOARD=m5stick        (or ATOM_PORT=/dev/serial/by-id/...)
# Mint override if not on the LAN bind:
#   export MINT_URL=http://192.168.13.221:3338

# a) console driver smoke:
cargo test --features live --test e2e -- --ignored --nocapture atom_console_smoke

# b) console money loop (the core wallet path, no RF):
cargo test --features live,payer --test e2e -- --ignored --nocapture console_relay_money_loop

# c) receive-direction relay onto a REAL Type-4 boltcard (card sandwiched, ACR fresh):
cargo test --features live,payer --test e2e -- --ignored --nocapture relay_token_over_the_air_via_boltcard

# d) RF smoke (card on the atom antenna):
cargo test --features live --test e2e -- --ignored --nocapture atom_rf_iso14443_smoke

# e) CE small-payload proof (no mint needed; ONE CE scenario — replug ACR first):
cargo test --features live --test e2e -- --ignored --nocapture relay_acr_emulated_small_payload

# f) CE full-token (EXPECTED fail-fast on the 52 B geometry assert; informative only):
NUCULA_CE_STRIP_DLEQ=1 cargo test --features live,payer --test e2e -- --ignored --nocapture relay_acr_emulated

# g) offline stash→drain (needs a MANUAL AP step mid-test):
cargo test --features live,payer --test e2e -- --ignored --nocapture relay_stashes_offline_then_drains_on_reconnect

# h) read-only preflight report (no repairs):
cargo test --features live --lib -- --ignored --nocapture preflight_report_readonly
```

CE rules for tonight (AGENTS.md): **one CE scenario per ACR power cycle** —
replug the reader between (e)/(f) and anything else; never call
`exit_card_emulation`; do not re-add `set_picc_operating_parameter` to CE
flows; one write per `present_ndef_image` is already enforced in code.

### C.4 Soak / boundary / dumps

```bash
cd rig
# overnight soak (8 h default; JSONL lands in rig/results/soak-<ts>.jsonl):
cargo run --features live,payer --example soak -- 8
# analyze afterwards (works on any number of slices):
python3 rig/scripts/analyze_soak.py rig/results

# ~790-sat persistence boundary probe (10 × 42-sat receives + wstat):
cargo run --features live,payer --example boundary_probe

# long console capture (e.g. full tag dump):
cargo run --features live --example console_dump -- nfcdump 68

# M5Stick console recovery (bridge wedge):
python3 rig/scripts/recover_console.py 120

# ACR wedge reproducer — WARNING: burns the reader's power cycle,
# do NOT run before a CE presentation:
python3 rig/scripts/acr_instrumented.py
```

### C.5 Tonight's build order (from the gap map)

1. Recreate `/tmp/opencode/rig/start-mint.sh` + bring the mint up.
2. `console_relay_money_loop` on Atom, then `NUCULA_BOARD=m5stick` on the
   M5Stick (goal iii baseline).
3. `relay_token_over_the_air_via_boltcard` on the T4 boltcard (goal i,
   NTAG424 interpretation); N-iteration loop harness on top.
4. CE small-payload once per fresh ACR cycle (regression of the wedge rules).
5. Kick off `soak` overnight (goal iv; add `wstat` to the snapshot cadence
   first if time permits).
6. Draft the parity matrix (goal v) from §A.1 + `mint info`.

---

## D) Audit session log (environment facts a fresh operator needs)

- `cd rig && cargo test` (rustup stable 1.99.0): **9 passed, 0 failed,
  0 ignored** — `ndef_t2t::tests` (7) + `locks::tests` (2). Integration
  target `tests/e2e.rs` compiled empty (0 tests) because `live` is off;
  doc-tests 0. `payer`/`strip` units need `--features payer` and were not
  part of the plain run.
- System `cargo` 1.75.0 cannot parse `rig/Cargo.lock` (version 4) —
  `export PATH="$HOME/.cargo/bin:$PATH"` is mandatory.
- `rig/Cargo.toml` path-deps `cdk`/`cdk-sqlite` point at
  `/home/ubuntu/src/cdk/crates/…`, absent on this host; bridged with
  symlink `/home/ubuntu/src/cdk -> /root/src/cdk-0.18.1` (workspace
  version 0.18.1 vs lock's 0.18.0 → first resolve rewrites
  `rig/Cargo.lock`; restored via `git restore` after the audit run —
  tree is clean except this file).
- `/tmp/opencode/rig/start-mint.sh` (README:130) does not exist on this
  host — must be recreated before live runs.
- Build artifacts were kept out of the tree via
  `CARGO_TARGET_DIR=/tmp/opencode/rig-target` (rig/target/ is gitignored
  anyway).
- Hardware untouched: no flashing, no serial/pcsc device opens, no
  labgrid calls, no registry changes.
