# Roadmap & Upstream Contribution Plan

*Generated 2026-09-07 from the current state of `Amperstrand/nucula` and `Amperstrand/micronuts` (micronuts vendored at `src/micronuts/`).*

---

## 2026-09-13 refresh — the upstream queue as one-issue-per-thing

Everything below Section 1 predates the M5Stick target, the live CE
wins, and the soak campaign; it remains accurate on mechanics (rebase,
squash plan, commit anchors) but the ordering and gating have moved on.
Current rules:

- **Only proven-on-hardware work ships upstream.** Proven now: Atom +
  M5Stick console + wallet (soak: hundreds of full-token money loops),
  RC522 T2T read chain, CE small-payload relay e2e, redeem-or-stash.
  Not yet proven: M5Stick display (panel currently off on our rig),
  T4T against real cards/phones — those wait for their own PRs.
- **The queue is tracked as issues on this fork** (#3–#11) so the
  maintainer can consume our branch as context and reimplement in their
  own style where preferred:
  #3 LICENSE → #4 NDEF NLEN fix (trust-builder) → #5 console fixes →
  #6 board framework + Atom → #7 rc522 driver → #8 reader frontend +
  nfc_common + T2T fallback → #9 host test infra (the separate
  "testing framework" MR) → #10 M5Stick (keypad-less by design —
  probe-and-disable peripherals + USB console REPL covers seed entry
  and every op; button/display UI is a follow-up) → #11 ACR1252U CE
  findings as an evidence issue.
- **Not for upstream, ever:** `rig/` (lab-specific), the DLEQ stripper
  (contradicts the NUT-12 requirement; it is a rig tool for constrained
  carriers), WiFi credentials, labgrid configs.
- Upstream is quiet since 2026-07-20 and has no CONTRIBUTING.md —
  mirror the style of merged upstream PRs (e.g. `8ad0812`), keep PRs
  tiny, and let the issue queue carry the context load.

---

## Current State

**Done and verified:**

- **M5Stack Atom port is working end-to-end at the rig level**: RC522 I2C driver (ISO-DEP + Type 2/4 NDEF), reader-mode NFC frontend with T2T fallback, UART0 console transport, AN10217 stuck-bus recovery, and a full host-test battery (28 NFC tests + 6 wallet suites).
- **Spec-quote discipline in both repos**: `main/specquotes.toml` in nucula (verbatim NUT quotes in `// NUT #XX:` comments) and `specquotes.toml` in cashu-core-lite — verifiable with `greatspectate check`.
- **DLEQ stripping implemented and tested** (rig-side CBOR walker, `95433f9`).
- **T2T fallback implemented and flashed** (`07ce125`).
- **CDK interop deviation documented** (micronuts `4447801`).
- **Micronuts fork has 3 clean, upstreamable commits** (NUT-06 shapes fix, adapter bind env, DLEQ deviation doc).
- **ACR relay test script is ready** — blocked only on a physical replug.

**Blocked / waiting:**

- ACR relay e2e run: blocked on replugging the ACR (recover from its CE wedge) at the rig.
- Card relay: waiting on the bigger boltcard (if available) for the emulated-area test.
- RF coupling: antennas must be separated by a few cm before any OTA relay attempt.
- Upstream PRs: not yet started — branches need a clean rebase off `zeugmaster/nucula` main first.
- CDK DLEQ issue: needs human review before filing (AGENTS.md policy rule 1).

---

## Section 1: Upstream Contribution Strategy

### 1a. For nucula (`zeugmaster/nucula` — PRs from `Amperstrand/nucula`)

The fork carries 36+ commits on top of upstream main (fork point: `8ad0812` "Merge pull request #6 from zeugmaster/refactor/structure-campaign"). For a clean PR, split into two branches:

#### Branch 1: `feat/m5stack-atom-target` — the core port

| Path | Content |
|---|---|
| `components/rc522/` | MFRC522 I2C driver + ISO-DEP transport + T2T/T4T NDEF reading |
| `main/nfc_reader.cpp` | Reader-mode NFC frontend with T2T fallback |
| `main/nfc_common.{h,cpp}` | Extracted redeem/stash logic shared by both NFC frontends |
| `main/Kconfig.projbuild` | Board choice (`NUCULA_BOARD_*`) |
| `main/board.h` | ATOM pin definitions |
| `main/console.cpp` | UART0 transport for classic ESP32 |
| `main/i2c_bus.{c,h}` | AN10217 stuck-bus recovery before bus init |
| `main/ndef.cpp` | NDEF engine (shared) |
| `main/commands_system.cpp` | Diagnostics commands (`i2cscan`/`i2cdump`/`i2crecover`/`nfcdump`) |
| `main/wallet_blind.cpp` | `blank_output_count` relocation (pure TU — testability) |
| `main/specquotes.toml` + spec-quote comments | Spec-quote pins for the C++ core |
| `tests/host/` | Host test infra + 28 NFC tests + 6 wallet suites |
| `sdkconfig.defaults.atom` | Atom board sdkconfig defaults |

**NOT included** (keep local): `rig/` (lab-specific), labgrid configs, WiFi credentials, `.omo/`.

Relevant commit anchors on the current fork: `1f28c4b` (board target), `797a34c` (rc522 driver), `6a4fb06` (UART0), `d4848d1` (AN10217 recovery), `6341a0e` (reader frontend), `3b2967c` (diagnostics), `aad2129` (blank_output_count refactor), `3ebc5fa` + `521ef19` (host tests), `1199134` (spec-quotes), `07ce125` (T2T fallback), plus rc522 hardening `76adb40`/`669a9da`.

#### Branch 2: `test/host-build` — could be merged into the port or separate

- `tests/host/` infrastructure (`tests/host/CMakeLists.txt`, `test_main.cpp`, `nfc_tests.cpp`, `nvs_shim.cpp`, `shims/`)
- **The NLEN-form parse fix** — this is a bug fix that benefits **ALL targets**, not just the Atom. It was caught by the host NFC suite (`521ef19`). Strong candidate for its own fast-track PR if upstream prefers small fixes first.

#### Approach

1. **Rebase onto upstream main** (`zeugmaster/nucula`).
2. **Squash diagnostic iterations into logical commits** — target ~8-10 commits: (a) board target + pins, (b) rc522 driver, (c) I2C recovery + diagnostics, (d) reader-mode frontend, (e) T2T fallback, (f) nfc_common extraction, (g) wallet_blind refactor, (h) host test infra, (i) NFC/wallet test suites, (j) spec-quotes.
3. **Write the PR description** emphasizing: adds a second target board; reader-mode NFC **complements** the XIAO's card emulation (not a replacement); the core logic becomes host-testable; follows upstream code style.
4. **Check upstream `CONTRIBUTING.md`** — checked 2026-09-07: **none exists** at `zeugmaster/nucula` main (raw fetch → 404). Fall back to mirroring the style of merged upstream PRs (e.g. the structure-campaign merge `8ad0812`).
5. **The `wallet_blind.cpp` `blank_output_count` relocation** is a small refactor that helps testing — include it, but call it out explicitly in the PR description so reviewers see it's mechanical, not behavioral.

### 1b. For micronuts

Fork has 3 commits ahead:

| Commit | Subject |
|---|---|
| `c06c2f2` | `fix(audit-adapter): emit NUT-06 nut shapes cdk HEAD deserializes` |
| `41d08fa` | `feat(adapter): MICRONUTS_ADAPTER_BIND env for LAN exposure` |
| `4447801` | `docs: CDK CBOR DLEQ interop deviation + spec-quote aside` |

- **The NUT-06 fix (`c06c2f2`) is directly PR-able** to cashubtc/cdk — note the adapter itself lives in the micronuts repo, so confirm the upstream landing spot (cdk-side serialization change vs. a micronuts-side patch) before opening the PR.
- **The DLEQ interop doc (`4447801`) is internal documentation.** The corresponding upstream issue must go through **human review first** per AGENTS.md policy rule 1 — do not file it autonomously.
- `41d08fa` (bind env) is demo-tooling convenience; upstream value is marginal — hold unless the NUT-06 PR conversation opens a door for it.

---

## Section 2: M5Stick Board Target

The **M5StickC Plus (or Plus2)** is the natural third target:

- **ST7789 135x240 display** — bolty-rs has working driver code
- **2 buttons** (front + side) — enough for full wallet navigation
- **ESP32** (classic, same as the Atom) — no new toolchain
- **Grove port** — same I2C bus for the RC522
- **Battery** — the portable option

### From bolty-rs (reference code)

- Board config: `board-m5stick` feature in bolty-rs `Cargo.toml`
- Display: `mipidsi` + `embedded-graphics` in `bolty-rs/apps/bolty-esp32`
- Pin map: **SDA=GPIO33, SCL=GPIO32** (from the bolty-rs board config read earlier)
- Buttons: **GPIO37 (front)**, **GPIO39 (side)**

### Porting to nucula

1. **Kconfig**: add `NUCULA_BOARD_M5STICK` alongside the existing board choice in `main/Kconfig.projbuild`.
2. **Pins**: add M5Stick definitions in `main/board.h` — I2C on G32/G33 (per bolty-rs) or G25/G26 depending on the variant; confirm against the actual unit before wiring.
3. **Display driver**: the existing SSD1309 code adapts to ST7789 — both are SPI, different init sequences.
4. **UI**: two-button navigation — front = select/next, side = back/menu.
5. **NFC**: same RC522 frontend as the Atom (same driver, different I2C pins — the board.h abstraction absorbs the difference).
6. **Positioning**: the M5Stick is the **"display + button" version** of the wallet (the Atom Matrix is the minimal 5x5-LED version).

---

## Section 3: Today's Roadmap

### Immediate (today)

1. ✅ ACR relay test — **blocked on replug; script ready, run when at the rig**
2. ✅ DLEQ stripping implemented and tested
3. ✅ T2T fallback implemented and flashed
4. ✅ Spec-quotes added to both repos
5. ✅ CDK interop documented
6. **Physical actions needed at the rig:**
   - Replug the ACR (recover from CE wedge)
   - Swap in the bigger boltcard (if available) for the card relay
   - Separate antennas by a few cm
   - Then run:
     ```
     cargo test --features live,payer --test e2e -- --ignored --nocapture
     ```

### Next session

1. Re-run the ACR-as-tag relay test with the DLEQ-stripped token (after replug)
2. If successful: add the DLEQ-stripped ACR relay as a proper e2e test
3. Start the M5Stick board target (Kconfig + `board.h` + display init)
4. LED matrix driver with current limiting (for the Atom)
5. Micronuts host-test infrastructure (port the nucula `tests/host/` CMake + shims pattern)

### Upstream preparation

1. Create `feat/m5stack-atom-target` branch — clean rebase from upstream (`zeugmaster/nucula`), squashed per Section 1a
2. Create `feat/m5stick-target` branch — once M5Stick works
3. Micronuts NUT-06 fix (`c06c2f2`) → PR (cashubtc/cdk or the micronuts adapter's upstream home — confirm landing spot first)
4. CDK DLEQ CBOR deviation → file issue on cashubtc/cdk (**after human review** — AGENTS.md policy rule 1)
