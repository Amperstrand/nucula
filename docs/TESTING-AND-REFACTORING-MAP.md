# Testing & Refactoring Map — 2026-09-13

What exists, what's proven, what's next. Companion to
ROADMAP-AND-CONTRIBUTION.md (upstream queue) and micronuts#68 (the Rust
port plan).

## Testing inventory

| Layer | What | Status |
|---|---|---|
| Host unit | `tests/host`: 28 NDEF + 6 wallet suites | green, zero hardware |
| Rig e2e (money) | `console_relay_money_loop` | green on M5Stick (real mint, full tokens) |
| Rig e2e (NFC) | `relay_acr_emulated_small_payload` | green (CE→RF→RC522→NDEF, sub-52 B payloads) |
| Rig e2e (blocked) | `relay_acr_emulated` full-token | measured-blocked: Ultralight CE serves 16 pages |
| Soak | `rig/examples/soak.rs` + supervisor | 8 h campaign complete 2026-09-13: 88 slices, 822 green cycles, 84 wedge-absorbs, dense per-cycle balance telemetry |
| Forensics | `rig/scripts/analyze_soak.py` | balance series + decrease boundaries + fail context |
| Recovery | `rig/scripts/recover_console.py` | proven standalone; fails when invoked in-process (open question) |
| Bench contract | labgrid place `nucula-rig` (3 tokens), fips-lab registry `m5stick-nucula`, BenchLock | live |
| Upstream PRs | branches `upstream/{board-atom,rc522,reader-nfc,host-tests,m5stick,specquotes}` | built green standalone + stacked; tracked #6–#10 |

## Open test gaps (ordered)

1. **T4T against real carriers** — NTAG424 boltcard read + phone HCE
   (nucula#1 Session 2): the only unproven NFC leg.
2. **M5Stick display** — panel-off on our rig; needs a display session
   before the display PR exists.
3. **Crash-consistency forensics** — dense balance data captured; morning
   readout must confirm/refute the wallet-state-reset finding (the 732→589
   regrow arithmetic) and file the findings issue.
4. **In-process recovery** — why recover_console.py succeeds standalone
   but fails when spawned from the soak; suspected port-holder interplay.
5. **100-sat console probe** — deterministic output break with swap
   completing; framed-console prototype would close it (also micronuts#68
   input).

## Refactoring map

- **Done in fork**: nfc_common extraction (CE/reader share redeem+stash),
  blank_output_count TU relocation (testability), board Kconfig framework,
  console UART0 transport, spec-quote pins.
- **Upstream-prep done**: the six-branch stack (each one clean commit,
  standalone-verified where it matters: board-atom builds alone; tip
  builds for esp32/atom; host suites green).
- **Next refactors, in order**:
  1. M0 of micronuts#68 — promote bolty's MFRC522 crate family to shared
     crates (the single highest-leverage unblock for the Rust port).
  2. Framed console (length-prefixed or CBOR-RPC seam) — kills the 1.5 KB
     ceiling class entirely.
  3. Wallet-core extraction in micronuts (`engine/flow/state/payload` →
     `micronuts-wallet-core`) — M1 of the port plan.
  4. nucula display session (panel-on, visual QA) → unlocks the display PR.

## Field lessons bank (keep quoted)

Hades2001 bridge (115200 flash, explicit `--port`, daemon-stop,
pyserial-pulse recovery) · ACR CE one-write/one-power-cycle · Ultralight
CE 52-byte ceiling · esptool default-port trap (flashed the CYD silently)
· esp32c3 root-sdkconfig trap · `git add -A` in a repo with runtime
artifacts (cost: soak JSONLs, recovered from reflog).
