# M5Stick display bench session — 2026-10-17

Branch to flash: `amperstrand/m5stick-display` (pushed to the fork).
Build exactly as CI does:

```sh
. $HOME/esp/v5.5.1/export.sh   # or the container equivalent
idf.py -B build-m5stick -DSDKCONFIG=build-m5stick/sdkconfig -DIDF_TARGET=esp32 \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.m5stick" flash monitor
```

## Why this session exists

The glass has never been lit. The old code drove the backlight on
**GPIO27 — a Plus2 pinout**. Our unit is a **Plus v1** (proven: the
AXP192 EXTEN Grove power-cycles work on the bench), where the
backlight is **AXP192 LDO2**. The new driver auto-detects the variant
by probing for the PMU and routes the backlight accordingly. That is
the primary thing to confirm.

## Ordered checks

1. **Boot log** — expect `axp192: AXP192 ready (DCDC1+LDO2+LDO3+EXTEN on)`
   then `display: ST7789 135x240 ready (Plus variant, offset 52,40, inverted)`.
   `display variant` on the console must say `Plus (AXP192)`.
   If it reports `Plus2 (GPIO rails)` instead, the PMU probe failed —
   capture the boot log before power-cycling.
2. **Backlight** — glass lit from boot (LDO2 came up with rail init).
   `display off` / `display on` must actually darken/lighten. If `off`
   works but `on` doesn't (or vice versa), suspect LDO2 state vs the
   0x12 RMW path — log reg reads if needed.
3. **Panel sanity** — `display fill`, `display fill f800` (red),
   `display fill 001f` (blue): full-glass color. A shifted or wrapped
   image means the (52,40) gap or MADCTL needs work — photograph any
   misalignment for the PR notes.
4. **UI** — the home screen (balance/mints/wifi) renders top-left in
   5x7 mono. `status` on console, then the front button (A) — screen
   must refresh. Sparse layout on the 135x240 canvas is expected;
   layout polish is a follow-up, not a failure.
5. **The blanking anomaly** — side button (B) toggles the backlight.
   Repeatedly. If a press blanks the glass again, note whether
   backlight-level (console `display on` recovers) or panel-level
   (fill doesn't redraw) — that distinguishes PMU brownout from SPI
   corruption. `button` command reports which pad fired.
6. **RC522 still alive** — the PMU init moved earlier in boot (before
   the Grove bus probe). `i2cscan`, then an actual NFC receive:
   `nfc request <amount>` with the ACR/boltcard rig as usual.
7. **Money loop** — one receive + `wstat` if the PR-18 persistence
   branch is also flashed; on this branch just `receive`/`balance`.

## Capture for the PR

- Boot log excerpt (PMU + display lines), photos of fill + home
  screen, button-behavior note. These become the evidence section of
  the eventual `upstream/m5stick` PR description.

## Branch map (all on the fork)

- `amperstrand/m5stick-display` — flash this. 9 commits:
  S1 board framework → S2 rc522 → S3 reader frontend → S4 host tests →
  M1 M5Stick+AXP192 → M2 ST7789 esp_lcd → M3 UI backend → M4 buttons →
  M5 CI. Each builds standalone; CI runs host suites + 3 firmware
  targets on every push.
- `fix/15-persistence-boundary` — PR #18 (open, unmerged).
- The old `upstream/*` queue branches are now superseded by S1–S4 on
  this branch; after bench validation, re-point them here before any
  submission. Nothing has been submitted upstream.
